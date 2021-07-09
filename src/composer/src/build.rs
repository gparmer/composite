
use initargs::ArgsKV;
use passes::{component, deps, exports, BuildState, ComponentId, SystemState};
use std::fs::File;
use syshelpers::{dir_exists, emit_file, exec_pipeline, reset_dir};
use tar::Builder;
use std::env;

// Interact with the composite build system to "seal" the components.
// This requires linking them with all dependencies, and with libc,
// and making them executable programs (i.e. no relocatable objects).
//
// Create the build context for a component derived from its
// specification the sysspec, including 1. the interfaces it
// exports, 2. the interface dependencies it relies on, and 3. the
// libraries it requires.  The unexpected part of this is that
// each interface can have its own dependencies and library
// requirements, thus this must return the transitive closure over
// those dependencies.
//
// Note that the Makefiles of components and interfaces include a
// specification of all of this.  FIXME: We should check the
// specification in the syspec against these local (and
// compile-checked) specifications, and bomb out with an error if
// they don't match.  For now, a disparity will result in compiler
// errors, instead of errors here.  Thus, FIXME.
//
// The goal of the context is to build up all of this information,
// then call `make component` with the correct make variables
// initialized.  These include:
//
// - COMP_INTERFACES - list of '+'-separated exported
//   interfaces/variant pairs. For example "pong/stubs" for default,
//   or "pong/log" for a log variant
// - COMP_IFDEPS - list of '+'-separated interface dependencies and
//   variants, again specified as "if/variant"
// - COMP_LIBS - list of space separated library dependencies
// - COMP_INTERFACE - this component's interface directory
// - COMP_NAME - which component implementation to use
// - COMP_VARNAME - the name of the component's variable in the sysspec
// - COMP_BASEADDR - the base address of .text for the component
// - COMP_INITARGS_FILE - the path to the generated initial arguments .c file
// - COMP_TAR_FILE - the path to an initargs tarball to compile into the component
//
// In the end, this should result in a command line for each component
// along these (artificial) lines:
//
// `make COMP_INTERFACES="pong/log" COMP_IFDEPS="capmgr/stubs sched/lock" COMP_LIBS="ps heap" COMP_INTERFACE=pong COMP_NAME=pingpong COMP_VARNAME=pongcomp component`
//
// ...which should output the executable pong.pingpong.pongcomp in the
// build directory which is the "sealed" version of the component that
// is ready for loading.

// The key within the initargs for the tarball, the path of the
// tarball, and the set of paths to the files to include in the
// tarball and name of them within the tarball.
fn tarball_create(
    tarball_key: &String,
    tar_path: &String,
    contents: Vec<(String, String)>,
) -> Result<(), String> {
    let file = File::create(&tar_path).unwrap();
    let mut ar = Builder::new(file);
    let dir_template = env::current_dir().unwrap(); // just need *some* directory with read/write perms
    let key = format!("{}/", tarball_key);

    ar.append_dir(&key, &dir_template).unwrap(); // FIXME: error handling
    contents.iter().for_each(|(p, n)| {
        // file path, and name for the tarball
        let mut f = File::open(p).unwrap(); //  should not fail: we just built this, TODO: fix race
        ar.append_file(format!("{}/{}", tarball_key, n), &mut f)
            .unwrap(); // FIXME: error handling
    });
    ar.finish().unwrap(); // FIXME: error handling
    Ok(())
}

fn constructor_tarball_create(
    id: &ComponentId,
    s: &SystemState,
    b: &dyn BuildState,
) -> Result<Option<String>, String> {
    let me = component(&s, &id);
    let tar_path = b.comp_file_path(&id, &"initfs_constructor.tar".to_string(), &s)?;

    let tar_files: Vec<(String, String)> = s
        .get_named()
        .ids()
        .iter()
        .filter_map(|(cid, _name)| {
            let c = component(&s, &cid);
            // are we the constructor for this component?
            if me.name != c.constructor {
                return None;
            }

            Some((
                b.comp_obj_path(&cid, &s).unwrap(),
                b.comp_obj_file(&cid, &s),
            ))
        })
        .collect();
    if tar_files.len() == 0 {
        return Ok(None);
    }

    tarball_create(&"binaries".to_string(), &tar_path, tar_files)?;

    Ok(Some(tar_path))
}

fn constructor_serialize_args(
    id: &ComponentId,
    s: &SystemState,
    b: &dyn BuildState,
) -> Result<String, String> {
    let mut sinvs = Vec::new();
    let mut ids = Vec::new();

    for s in s.get_invs_id(id).invocations().iter() {
        let mut sinv = Vec::new();
        sinv.push(ArgsKV::new_key(String::from("name"), s.symb_name.clone()));
        sinv.push(ArgsKV::new_key(
            String::from("client"),
            String::from(format!("{}", s.client)),
        ));
        sinv.push(ArgsKV::new_key(
            String::from("server"),
            String::from(format!("{}", s.server)),
        ));
        sinv.push(ArgsKV::new_key(
            String::from("c_fn_addr"),
            String::from(format!("{}", s.c_fn_addr)),
        ));
        sinv.push(ArgsKV::new_key(
            String::from("c_ucap_addr"),
            String::from(format!("{}", s.c_ucap_addr)),
        ));
        sinv.push(ArgsKV::new_key(
            String::from("s_fn_addr"),
            String::from(format!("{}", s.s_fn_addr)),
        ));

        // Just an array of each of the maps for each sinv.  Arrays
        // have "_" keys (see initargs.h).
        sinvs.push(ArgsKV::new_arr(String::from("_"), sinv));
    }

    s.get_named().ids().iter().for_each(|(id, _cname)| {
        let info_addr = s.get_objs_id(&id).comp_symbs().comp_info;
        let cinfo = ArgsKV::new_arr(
            format!("{}", id),
            vec![
                ArgsKV::new_key("img".to_string(), b.comp_obj_file(&id, &s)),
                ArgsKV::new_key("info".to_string(), format!("{}", info_addr)),
            ],
        );
        ids.push(cinfo)
    });

    let mut topkv = Vec::new();
    topkv.push(ArgsKV::new_arr(String::from("sinvs"), sinvs));
    topkv.push(ArgsKV::new_arr(String::from("components"), ids));
    s.get_param_id(&id)
        .param_list()
        .iter()
        .for_each(|a| topkv.push(a.clone()));

    let top = ArgsKV::new_top(topkv);
    let args = top.serialize();

    let args_file_path = b.comp_file_path(&id, &"initargs_constructor.c".to_string(), &s)?;
    emit_file(&args_file_path, args.as_bytes()).unwrap();

    Ok(args_file_path)
}

fn comp_gen_make_cmd(
    output_name: &String,
    args_file: &String,
    tar_file: &Option<String>,
    id: &ComponentId,
    s: &SystemState,
) -> String {
    let c = component(&s, id);
    let ds = deps(&s, id);
    let exports = exports(&s, id);

    let (_, if_exp) = exports
        .iter()
        .fold((true, String::from("")), |(first, accum), e| {
            let mut ifpath = accum.clone();
            if !first {
                ifpath.push_str("+");
            }
            ifpath.push_str(&e.interface.clone());
            ifpath.push_str("/");
            ifpath.push_str(&e.variant.clone());
            (false, ifpath)
        });
    let (_, if_deps) = ds
        .iter()
        .fold((true, String::from("")), |(first, accum), d| {
            let mut ifpath = accum.clone();
            if !first {
                ifpath.push_str("+");
            }
            ifpath.push_str(&d.interface.clone());
            ifpath.push_str("/");
            ifpath.push_str(&d.variant.clone());
            (false, ifpath)
        });

    let mut optional_cmds = String::from("");
    optional_cmds.push_str(&format!("COMP_INITARGS_FILE={} ", args_file));
    if let Some(s) = tar_file {
        optional_cmds.push_str(&format!("COMP_TAR_FILE={} ", s));
    }
    let decomp: Vec<&str> = c.source.split(".").collect();
    assert!(decomp.len() == 2);
    let name = format!("{}.{}", &c.name.scope_name, &c.name.var_name);

    let cmd = format!(r#"make -C src COMP_INTERFACES="{}" COMP_IFDEPS="{}" COMP_LIBDEPS="" COMP_INTERFACE={} COMP_NAME={} COMP_VARNAME={} COMP_OUTPUT={} COMP_BASEADDR={} {} component"#,
                      if_exp, if_deps, &decomp[0], &decomp[1], &name, output_name, &c.base_vaddr, &optional_cmds);

    cmd
}

fn kern_gen_make_cmd(input_constructor: &String, kern_output: &String, _s: &SystemState) -> String {
    format!(
        r#"make -C src KERNEL_OUTPUT="{}" CONSTRUCTOR_COMP="{}" plat"#,
        kern_output, input_constructor
    )
}

pub struct DefaultBuilder {
    builddir: String,
}

impl DefaultBuilder {
    pub fn new() -> Self {
        DefaultBuilder {
            builddir: "/dev/null".to_string(), // must initialize, so error out if you don't
        }
    }
}

fn compdir_check_build(comp_dir: &String) -> Result<(), String> {
    if !dir_exists(&comp_dir) {
        reset_dir(&comp_dir)?;
    }
    assert!(dir_exists(&comp_dir));

    Ok(())
}

impl BuildState for DefaultBuilder {
    fn initialize(&mut self, name: &String, _s: &SystemState) -> Result<(), String> {
        let pwd = env::current_dir().unwrap();
        let dir = format!("{}/system_binaries/cos_build-{}", pwd.display(), name);

        reset_dir(&dir)?;
        self.builddir = dir;

        Ok(())
    }

    fn file_path(&self, file: &String) -> Result<String, String> {
        Ok(format!("{}/{}", self.builddir, file))
    }

    fn comp_dir_path(&self, c: &ComponentId, state: &SystemState) -> Result<String, String> {
        let name = state.get_named().ids().get(c).unwrap();
        Ok(self.file_path(&format!("{}.{}", name.scope_name, name.var_name))?)
    }

    fn comp_file_path(
        &self,
        c: &ComponentId,
        file: &String,
        state: &SystemState,
    ) -> Result<String, String> {
        let comp_dir = self.comp_dir_path(&c, &state)?;
        compdir_check_build(&comp_dir)?;

        Ok(format!("{}/{}", comp_dir, file))
    }

    fn comp_obj_file(&self, c: &ComponentId, s: &SystemState) -> String {
        let comp = component(&s, &c);
        format!(
            "{}.{}.{}",
            &comp.source, &comp.name.scope_name, &comp.name.var_name
        )
    }

    fn comp_obj_path(&self, c: &ComponentId, s: &SystemState) -> Result<String, String> {
        self.comp_file_path(&c, &self.comp_obj_file(&c, &s), &s)
    }

    fn comp_build(&self, id: &ComponentId, state: &SystemState) -> Result<String, String> {
        let comp_dir = self.comp_dir_path(&id, &state)?;
        compdir_check_build(&comp_dir)?;
        let p = state.get_param_id(&id);
        let output_path = self.comp_obj_path(&id, &state)?;

        let cmd = comp_gen_make_cmd(&output_path, p.param_prog(), p.param_fs(), &id, &state);

        let name = state.get_named().ids().get(id).unwrap();
        println!(
            "Compiling component {}.{} with the following command line:\n\t{}",
            name.scope_name, name.var_name, cmd
        );

        let (out, err) = exec_pipeline(vec![cmd.clone()]);
        let comp_log = self.comp_file_path(&id, &"compilation.log".to_string(), &state)?;
        emit_file(
            &comp_log,
            format!(
                "Command: {}\nCompilation output:{}\nComponent compilation errors:{}",
                cmd, out, err
            )
            .as_bytes(),
        )?;
        if err.len() != 0 {
            println!(
                "Errors in compiling component {}. See {}.",
                &output_path, comp_log
            )
        }

        Ok(output_path)
    }

    fn constructor_build(&self, c: &ComponentId, s: &SystemState) -> Result<String, String> {
        let comp_dir = self.comp_dir_path(&c, &s)?;
        compdir_check_build(&comp_dir)?;

        let binary = self.comp_obj_path(&c, &s)?;
        let argsfile = constructor_serialize_args(&c, &s, self)?;
        let tarfile = constructor_tarball_create(&c, &s, self)?;
        let cmd = comp_gen_make_cmd(&binary, &argsfile, &tarfile, &c, &s);

        let name = s.get_named().ids().get(c).unwrap();
        println!(
            "Compiling component {}.{} with the following command line:\n\t{}",
            name.scope_name, name.var_name, cmd
        );

        let (out, err) = exec_pipeline(vec![cmd.clone()]);
        let comp_log = self.comp_file_path(&c, &"constructor_compilation.log".to_string(), &s)?;
        emit_file(
            &comp_log,
            format!(
                "Command: {}\nConstructor compilation output:{}\nComponent compilation errors:{}",
                cmd, out, err
            )
            .as_bytes(),
        )?;
        if err.len() != 0 {
            println!(
                "Errors in compiling component {}. See {}.",
                &binary, comp_log
            )
        }

        Ok(binary.clone())
    }

    fn kernel_build(
        &self,
        kern_output: &String,
        constructor_input: &String,
        s: &SystemState,
    ) -> Result<(), String> {
        let cmd = kern_gen_make_cmd(&constructor_input, &kern_output, &s);
        println!(
            "Compiling the kernel the following command line:\n\t{}",
            cmd
        );

        let (out, err) = exec_pipeline(vec![cmd.clone()]);
        let comp_log = self.file_path(&"kernel_compilation.log".to_string())?;
        emit_file(
            &comp_log,
            format!(
                "Command: {}\nKernel compilation output:{}\nKernel compilation errors:{}",
                cmd, out, err
            )
            .as_bytes(),
        )?;
        if err.len() != 0 {
            println!("Errors in compiling kernel. See {}.", comp_log)
        }

        Ok(())
    }
}
