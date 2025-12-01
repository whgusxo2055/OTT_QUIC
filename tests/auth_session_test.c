#include "auth/hash.h"
#include "auth/session.h"
#include "db/database.h"
#include "server/http.h"

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

    char sid[SESSION_ID_LEN + 1];
    assert(session_login(&ctx, "bob", "pass", sid, sizeof(sid)) == 0);

    int found_user = 0;
    assert(session_validate_and_extend(&ctx, sid, 60, &found_user) == 0);
    assert(found_user == user_id);

    assert(session_logout(&ctx, sid) == SQLITE_OK);
    db_session_t session;
    assert(db_get_session(&ctx, sid, &session) == SQLITE_NOTFOUND);

    db_close(&ctx);
}

static void test_header_extraction(void) {
    http_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.headers[0].name, "authorization");
    strcpy(req.headers[0].value, "Bearer TOKEN123");
    req.header_count = 1;

    char sid[64];
    assert(session_extract_from_headers(&req, sid, sizeof(sid)) == 0);
    assert(strcmp(sid, "TOKEN123") == 0);

    memset(&req, 0, sizeof(req));
    strcpy(req.headers[0].name, "cookie");
    strcpy(req.headers[0].value, "foo=1; SID=COOKIE123; bar=2");
    req.header_count = 1;
    assert(session_extract_from_headers(&req, sid, sizeof(sid)) == 0);
    assert(strcmp(sid, "COOKIE123") == 0);
}

int main(void) {
    test_password_hash();
    test_session_lifecycle();
    test_header_extraction();
    puts("auth_session_test passed");
    return 0;
}
