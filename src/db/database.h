#ifndef DB_DATABASE_H
#define DB_DATABASE_H

#include <sqlite3.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DB_MAX_USERNAME     64
#define DB_MAX_NICKNAME     64
#define DB_MAX_TITLE        128
#define DB_MAX_DESCRIPTION  512
#define DB_MAX_PATH         260
#define DB_MAX_TIMESTAMP    32
#define DB_MAX_THUMBNAIL    DB_MAX_PATH

typedef struct {
    sqlite3 *conn;
} db_context_t;

typedef struct {
    int id;
    int user_id;
    char session_id[65];
    char created_at[DB_MAX_TIMESTAMP];
    char expires_at[DB_MAX_TIMESTAMP];
} db_session_t;

typedef struct {
    int id;
    char username[DB_MAX_USERNAME];
    char nickname[DB_MAX_NICKNAME];
    char password_hash[DB_MAX_PATH];
    char created_at[DB_MAX_TIMESTAMP];
} db_user_t;

typedef struct {
    int id;
    char title[DB_MAX_TITLE];
    char description[DB_MAX_DESCRIPTION];
    char file_path[DB_MAX_PATH];
    char thumbnail_path[DB_MAX_THUMBNAIL];
    int duration;
    char upload_date[DB_MAX_TIMESTAMP];
} db_video_t;

typedef struct {
    int id;
    int user_id;
    int video_id;
    int last_position;
    char updated_at[DB_MAX_TIMESTAMP];
} db_watch_history_t;

int db_init(db_context_t *ctx, const char *db_path);
void db_close(db_context_t *ctx);
int db_initialize_schema(db_context_t *ctx);

int db_create_user(db_context_t *ctx,
                   const char *username,
                   const char *nickname,
                   const char *password_hash,
                   int *out_user_id);
int db_get_user_by_username(db_context_t *ctx, const char *username, db_user_t *out_user);
int db_delete_user_by_id(db_context_t *ctx, int user_id);

int db_create_video(db_context_t *ctx,
                    const char *title,
                    const char *description,
                    const char *file_path,
                    const char *thumbnail_path,
                    int duration,
                    int *out_video_id);
int db_get_video_by_id(db_context_t *ctx, int video_id, db_video_t *out_video);
int db_delete_video_by_id(db_context_t *ctx, int video_id);

int db_upsert_watch_history(db_context_t *ctx, int user_id, int video_id, int last_position);
int db_get_watch_history(db_context_t *ctx, int user_id, int video_id, db_watch_history_t *out_history);
int db_delete_watch_history(db_context_t *ctx, int user_id, int video_id);

int db_create_session(db_context_t *ctx, int user_id, const char *session_id, int ttl_seconds);
int db_get_session(db_context_t *ctx, const char *session_id, db_session_t *out_session);
int db_delete_session(db_context_t *ctx, const char *session_id);
int db_delete_expired_sessions(db_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // DB_DATABASE_H
