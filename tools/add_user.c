#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "auth/bcrypt.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <db_path> <username> <password>\n", argv[0]);
        return 1;
    }

    const char *db_path = argv[1];
    const char *username = argv[2];
    const char *password = argv[3];

    char salt[BCRYPT_HASHSIZE];
    char hash[BCRYPT_HASHSIZE];

    if (bcrypt_gensalt(12, salt) != 0) {
        fprintf(stderr, "Failed to generate salt\n");
        return 1;
    }

    if (bcrypt_hashpw(password, salt, hash) != 0) {
        fprintf(stderr, "Failed to hash password\n");
        return 1;
    }

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Check if user exists
    const char *check_sql = "SELECT id FROM users WHERE username = ?;";
    sqlite3_stmt *check_stmt;
    if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare check statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }
    sqlite3_bind_text(check_stmt, 1, username, -1, SQLITE_STATIC);
    if (sqlite3_step(check_stmt) == SQLITE_ROW) {
        printf("User '%s' already exists. Skipping.\n", username);
        sqlite3_finalize(check_stmt);
        sqlite3_close(db);
        return 0;
    }
    sqlite3_finalize(check_stmt);

    const char *sql = "INSERT INTO users (username, nickname, password_hash) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC); // Use username as nickname
    sqlite3_bind_text(stmt, 3, hash, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    printf("User '%s' added successfully.\n", username);
    return 0;
}
