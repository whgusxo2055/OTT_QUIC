#include "auth/hash.h"
#include "db/database.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    db_context_t ctx;
    int rc = db_init(&ctx, ":memory:");
    assert(rc == SQLITE_OK);

    rc = db_initialize_schema(&ctx);
    assert(rc == SQLITE_OK);

    char hash[HASH_HEX_SIZE] = {0};
    hash_password("secret", hash, sizeof(hash));

    int user_id = 0;
    rc = db_create_user(&ctx, "alice", "Alice", hash, &user_id);
    assert(rc == SQLITE_OK);
    assert(user_id > 0);

    db_user_t user;
    rc = db_get_user_by_username(&ctx, "alice", &user);
    assert(rc == SQLITE_OK);
    assert(user.id == user_id);

    int video_id = 0;
    rc = db_create_video(&ctx,
                         "Sample Video",
                         "Example description",
                         "/videos/sample.mp4",
                         "/thumbs/sample.jpg",
                         120,
                         &video_id);
    assert(rc == SQLITE_OK);
    assert(video_id > 0);

    db_video_t video;
    rc = db_get_video_by_id(&ctx, video_id, &video);
    assert(rc == SQLITE_OK);
    assert(video.id == video_id);

    rc = db_upsert_watch_history(&ctx, user_id, video_id, 42);
    assert(rc == SQLITE_OK);

    db_watch_history_t history;
    rc = db_get_watch_history(&ctx, user_id, video_id, &history);
    assert(rc == SQLITE_OK);
    assert(history.last_position == 42);

    rc = db_upsert_watch_history(&ctx, user_id, video_id, 84);
    assert(rc == SQLITE_OK);

    rc = db_get_watch_history(&ctx, user_id, video_id, &history);
    assert(rc == SQLITE_OK);
    assert(history.last_position == 84);

    rc = db_delete_watch_history(&ctx, user_id, video_id);
    assert(rc == SQLITE_OK);

    rc = db_get_watch_history(&ctx, user_id, video_id, &history);
    assert(rc == SQLITE_NOTFOUND);

    rc = db_delete_video_by_id(&ctx, video_id);
    assert(rc == SQLITE_OK);

    rc = db_delete_user_by_id(&ctx, user_id);
    assert(rc == SQLITE_OK);

    db_close(&ctx);
    puts("db_test passed");
    return 0;
}
