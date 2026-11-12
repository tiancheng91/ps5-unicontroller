#include "../../src/web/json.h"
#include <assert.h>
int main(void)
{
    Json j;
    const char *valid[] = {"{}", "[]", "{\"x\": [1,-2,3]}", "true", "null", "\"escaped\\\"text\""};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        assert(jparse(&j, valid[i], strlen(valid[i])) == 0);
    const char *bad[] = {"{",  "[1,]", "{\"x\":1,}", "{\"x\" 1}", "01",
                         "1.", "1e",   "falsex",     "{}x",       "\"\\q\""};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(jparse(&j, bad[i], strlen(bad[i])) < 0);
    const char *keys[] = {"x"};
    assert(jparse(&j, "{\"x\":1,\"x\":2}", 13) == 0);
    assert(jobject(&j, keys, 1) < 0);
    assert(jparse(&j, "{\"x\":18446744073709551616}", 26) == 0);
    int64_t value;
    assert(jint(&j, jget(&j, "x"), 0, INT64_MAX, &value) < 0);
    assert(jparse(&j, "{\"x\":true}", 10) == 0);
    int b;
    assert(jbool(&j, jget(&j, "x"), &b) == 0 && b);
    return 0;
}
