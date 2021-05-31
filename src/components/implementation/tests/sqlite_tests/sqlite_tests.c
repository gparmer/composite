#include <cos_component.h>
#include <llprint.h>
#include <sqlite3.h>
#include <cos_defkernel_api.h>

void
cos_init(void)
{
}

int result_print(void *v, int argc, char **argv, 
                    char **col_name) {
    for (int i = 0; i < argc; i++) {
        printc("%s = %s\n", col_name[i], argv[i] ? argv[i] : "NULL");
    }
    
    return 0;
}

int
main(void)
{
	printc("Calling sqlite functions\n");

    sqlite3 *db;
    char *err_msg = 0;
    char *sql = "CREATE TABLE TestTable(Id INTEGER PRIMARY KEY, Value TEXT);"
    "INSERT INTO TestTable(Value) VALUES ('TestItem1');"
    "INSERT INTO TestTable(Value) VALUES ('TestItem2');"
    "INSERT INTO TestTable(Value) VALUES ('TestItem3');"
    "INSERT INTO TestTable(Value) VALUES ('TestItem4');"
    "INSERT INTO TestTable(Value) VALUES ('TestItem5');";

    sqlite3_open(":memory:", &db);

    sqlite3_exec(db, sql, 0, 0, &err_msg);
    
	sql = "SELECT * FROM TestTable WHERE Id == 3";
	sqlite3_exec(db, sql, result_print, 0, &err_msg);

    sqlite3_close(db);

	while(1);
}
