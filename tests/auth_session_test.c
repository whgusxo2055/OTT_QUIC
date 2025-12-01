#include "auth/hash.h"
#include "db/database.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_password_hash(void) {
    char hash[HASH_HEX_SIZE];
    memset(hash, 0, sizeof(hash));
    hash_password("secret", hash, sizeof(hash));
    assert(strlen(hash) > 0);
    assert(verify_password("secret", hash) == 0);
    assert(verify_password("wrong", hash) != 0);
}

static void test_session_lifecycle(void) {
    db_context_t ctx;
    assert(db_init(&ctx, ":memory:") == SQLITE_OK);
    assert(db_initialize_schema(&ctx) == SQLITE_OK);

    int user_id = 0;
    char hash[HASH_HEX_SIZE];
    hash_password("pass", hash, sizeof(hash));
    assert(db_create_user(&ctx, "bob", "Bob", hash, &user_id) == SQLITE_OK);

    assert(db_create_session(&ctx, user_id, "SID123", 60) == SQLITE_OK);

    db_session_t session;
    assert(db_get_session(&ctx, "SID123", &session) == SQLITE_OK);
    assert(session.user_id == user_id);

    assert(db_delete_session(&ctx, "SID123") == SQLITE_OK);
    assert(db_get_session(&ctx, "SID123", &session) == SQLITE_NOTFOUND);

    db_close(&ctx);
}

int main(void) {
    test_password_hash();
    test_session_lifecycle();
    puts("auth_session_test passed");
    return 0;
}
