#include "db/database.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void copy_column_text(sqlite3_stmt *stmt, int column, char *dest, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }

    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (!text) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, (const char *)text, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static int exec_sql(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error (%d): %s\nSQL: %s\n", rc, errmsg ? errmsg : "unknown", sql);
        sqlite3_free(errmsg);
    }
    return rc;
}

static int ensure_users_role_column(sqlite3 *db) {
    const char *pragma_sql = "PRAGMA table_info(users);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, pragma_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    int has_role = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && strcasecmp((const char *)name, "role") == 0) {
            has_role = 1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return rc;
    }

    if (has_role) {
        return SQLITE_OK;
    }

    const char *alter_sql =
        "ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user' "
        "CHECK(role IN ('user','admin'));";
    return exec_sql(db, alter_sql);
}

int db_init(db_context_t *ctx, const char *db_path) {
    if (!ctx || !db_path) {
        return SQLITE_MISUSE;
    }

    memset(ctx, 0, sizeof(*ctx));
    int rc = sqlite3_open(db_path, &ctx->conn);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to open DB: %s\n", sqlite3_errmsg(ctx->conn));
        sqlite3_close(ctx->conn);
        ctx->conn = NULL;
        return rc;
    }

    return exec_sql(ctx->conn, "PRAGMA foreign_keys = ON;");
}

void db_close(db_context_t *ctx) {
    if (!ctx || !ctx->conn) {
        return;
    }

    sqlite3_close(ctx->conn);
    ctx->conn = NULL;
}

int db_initialize_schema(db_context_t *ctx) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }

    const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT NOT NULL UNIQUE,"
        " nickname TEXT NOT NULL,"
        " password_hash TEXT NOT NULL,"
        " role TEXT NOT NULL DEFAULT 'user' CHECK(role IN ('user','admin')),"
        " created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " user_id INTEGER NOT NULL,"
        " session_id TEXT NOT NULL UNIQUE,"
        " created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        " expires_at DATETIME NOT NULL,"
        " FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS videos ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " title TEXT NOT NULL,"
        " description TEXT,"
        " file_path TEXT NOT NULL,"
        " thumbnail_path TEXT,"
        " segment_path TEXT,"
        " duration INTEGER DEFAULT 0,"
        " upload_date DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS watch_history ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " user_id INTEGER NOT NULL,"
        " video_id INTEGER NOT NULL,"
        " last_position INTEGER NOT NULL DEFAULT 0,"
        " updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        " UNIQUE(user_id, video_id),"
        " FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
        " FOREIGN KEY(video_id) REFERENCES videos(id) ON DELETE CASCADE"
        ");";

    int rc = exec_sql(ctx->conn, schema_sql);
    if (rc != SQLITE_OK) {
        return rc;
    }
    return ensure_users_role_column(ctx->conn);
}

static int bind_optional_text(sqlite3_stmt *stmt, int idx, const char *text) {
    if (text) {
        return sqlite3_bind_text(stmt, idx, text, -1, SQLITE_TRANSIENT);
    }
    return sqlite3_bind_null(stmt, idx);
}

int db_create_user(db_context_t *ctx,
                   const char *username,
                   const char *nickname,
                   const char *password_hash,
                   const char *role,
                   int *out_user_id) {
    if (!ctx || !ctx->conn || !username || !nickname || !password_hash) {
        return SQLITE_MISUSE;
    }

    const char *role_value = (role && *role) ? role : "user";

    const char *sql =
        "INSERT INTO users (username, nickname, password_hash, role) VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, nickname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, password_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, role_value, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        if (out_user_id) {
            *out_user_id = (int)sqlite3_last_insert_rowid(ctx->conn);
        }
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_get_user_by_username(db_context_t *ctx, const char *username, db_user_t *out_user) {
    if (!ctx || !ctx->conn || !username || !out_user) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "SELECT id, username, nickname, password_hash, role, IFNULL(created_at, '')"
        " FROM users WHERE username = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(out_user, 0, sizeof(*out_user));
        out_user->id = sqlite3_column_int(stmt, 0);
        copy_column_text(stmt, 1, out_user->username, sizeof(out_user->username));
        copy_column_text(stmt, 2, out_user->nickname, sizeof(out_user->nickname));
        copy_column_text(stmt, 3, out_user->password_hash, sizeof(out_user->password_hash));
        copy_column_text(stmt, 4, out_user->role, sizeof(out_user->role));
        copy_column_text(stmt, 5, out_user->created_at, sizeof(out_user->created_at));
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_get_user_by_id(db_context_t *ctx, int user_id, db_user_t *out_user) {
    if (!ctx || !ctx->conn || !out_user) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "SELECT id, username, nickname, password_hash, role, IFNULL(created_at, '')"
        " FROM users WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(out_user, 0, sizeof(*out_user));
        out_user->id = sqlite3_column_int(stmt, 0);
        copy_column_text(stmt, 1, out_user->username, sizeof(out_user->username));
        copy_column_text(stmt, 2, out_user->nickname, sizeof(out_user->nickname));
        copy_column_text(stmt, 3, out_user->password_hash, sizeof(out_user->password_hash));
        copy_column_text(stmt, 4, out_user->role, sizeof(out_user->role));
        copy_column_text(stmt, 5, out_user->created_at, sizeof(out_user->created_at));
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_delete_user_by_id(db_context_t *ctx, int user_id) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }

    const char *sql = "DELETE FROM users WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_create_video(db_context_t *ctx,
                    const char *title,
                    const char *description,
                    const char *file_path,
                    const char *thumbnail_path,
                    const char *segment_path,
                    int duration,
                    int *out_video_id) {
    if (!ctx || !ctx->conn || !title || !file_path) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "INSERT INTO videos (title, description, file_path, thumbnail_path, segment_path, duration)"
        " VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 2, description);
    sqlite3_bind_text(stmt, 3, file_path, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 4, thumbnail_path);
    bind_optional_text(stmt, 5, segment_path);
    sqlite3_bind_int(stmt, 6, duration);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        if (out_video_id) {
            *out_video_id = (int)sqlite3_last_insert_rowid(ctx->conn);
        }
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_get_video_by_id(db_context_t *ctx, int video_id, db_video_t *out_video) {
    if (!ctx || !ctx->conn || !out_video) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "SELECT id, title, IFNULL(description,''), file_path, IFNULL(thumbnail_path,''),"
        " IFNULL(segment_path,''), IFNULL(duration,0), IFNULL(upload_date,'') FROM videos WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, video_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(out_video, 0, sizeof(*out_video));
        out_video->id = sqlite3_column_int(stmt, 0);
        copy_column_text(stmt, 1, out_video->title, sizeof(out_video->title));
        copy_column_text(stmt, 2, out_video->description, sizeof(out_video->description));
        copy_column_text(stmt, 3, out_video->file_path, sizeof(out_video->file_path));
        copy_column_text(stmt, 4, out_video->thumbnail_path, sizeof(out_video->thumbnail_path));
        copy_column_text(stmt, 5, out_video->segment_path, sizeof(out_video->segment_path));
        out_video->duration = sqlite3_column_int(stmt, 6);
        copy_column_text(stmt, 7, out_video->upload_date, sizeof(out_video->upload_date));
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_delete_video_by_id(db_context_t *ctx, int video_id) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }

    const char *sql = "DELETE FROM videos WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, video_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_update_video_segment_path(db_context_t *ctx, int video_id, const char *segment_path) {
    if (!ctx || !ctx->conn || !segment_path) {
        return SQLITE_MISUSE;
    }

    const char *sql = "UPDATE videos SET segment_path = ? WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, segment_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, video_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_update_video_metadata(db_context_t *ctx, int video_id, const char *title, const char *description) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }
    const char *sql = "UPDATE videos SET title = ?, description = ? WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_bind_text(stmt, 1, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, description ? description : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, video_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return rc;
}

int db_upsert_watch_history(db_context_t *ctx, int user_id, int video_id, int last_position) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "INSERT INTO watch_history (user_id, video_id, last_position, updated_at)"
        " VALUES (?, ?, ?, CURRENT_TIMESTAMP)"
        " ON CONFLICT(user_id, video_id) DO UPDATE SET"
        "   last_position = excluded.last_position,"
        "   updated_at = CURRENT_TIMESTAMP;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, video_id);
    sqlite3_bind_int(stmt, 3, last_position);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_get_watch_history(db_context_t *ctx, int user_id, int video_id, db_watch_history_t *out_history) {
    if (!ctx || !ctx->conn || !out_history) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "SELECT id, user_id, video_id, last_position, IFNULL(updated_at,'')"
        " FROM watch_history WHERE user_id = ? AND video_id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, video_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(out_history, 0, sizeof(*out_history));
        out_history->id = sqlite3_column_int(stmt, 0);
        out_history->user_id = sqlite3_column_int(stmt, 1);
        out_history->video_id = sqlite3_column_int(stmt, 2);
        out_history->last_position = sqlite3_column_int(stmt, 3);
        copy_column_text(stmt, 4, out_history->updated_at, sizeof(out_history->updated_at));
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_delete_watch_history(db_context_t *ctx, int user_id, int video_id) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }

    const char *sql = "DELETE FROM watch_history WHERE user_id = ? AND video_id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, video_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_create_session(db_context_t *ctx, int user_id, const char *session_id, int ttl_seconds) {
    if (!ctx || !ctx->conn || !session_id) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "INSERT INTO sessions (user_id, session_id, expires_at) "
        "VALUES (?, ?, datetime('now', ?));";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    char ttl_str[32];
    snprintf(ttl_str, sizeof(ttl_str), "+%d seconds", ttl_seconds);

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ttl_str, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_get_session(db_context_t *ctx, const char *session_id, db_session_t *out_session) {
    if (!ctx || !ctx->conn || !session_id || !out_session) {
        return SQLITE_MISUSE;
    }

    const char *sql =
        "SELECT id, user_id, session_id, IFNULL(created_at,''), IFNULL(expires_at,'') "
        "FROM sessions WHERE session_id = ? AND expires_at > datetime('now');";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(out_session, 0, sizeof(*out_session));
        out_session->id = sqlite3_column_int(stmt, 0);
        out_session->user_id = sqlite3_column_int(stmt, 1);
        copy_column_text(stmt, 2, out_session->session_id, sizeof(out_session->session_id));
        copy_column_text(stmt, 3, out_session->created_at, sizeof(out_session->created_at));
        copy_column_text(stmt, 4, out_session->expires_at, sizeof(out_session->expires_at));
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_delete_session(db_context_t *ctx, const char *session_id) {
    if (!ctx || !ctx->conn || !session_id) {
        return SQLITE_MISUSE;
    }

    const char *sql = "DELETE FROM sessions WHERE session_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ctx->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
}

int db_delete_expired_sessions(db_context_t *ctx) {
    if (!ctx || !ctx->conn) {
        return SQLITE_MISUSE;
    }

    const char *sql = "DELETE FROM sessions WHERE expires_at <= datetime('now');";
    return exec_sql(ctx->conn, sql);
}
