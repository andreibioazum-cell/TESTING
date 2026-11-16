#include <jni.h>
#include <string>
#include "luac.h"

extern bool androidVulkanCreate(JNIEnv *env, jobject surface);
extern void androidVulkanDraw();
extern void androidVulkanDestroy();

extern "C" {
void lc_editor_init(void);
void lc_editor_free(void);
void lc_editor_set_text(const char *utf8);
const char *lc_editor_get_text(void);
void lc_editor_insert(const char *utf8);
void lc_editor_newline(void);
void lc_editor_backspace(void);
void lc_editor_delete(void);
void lc_editor_set_cursor_line_col(int line, int col);
void lc_editor_move_cursor(int dx, int dy);
int lc_editor_line_count(void);
int lc_editor_cursor_line(void);
int lc_editor_cursor_col(void);
int lc_editor_get_line(int line, char *out, int cap);
int lc_editor_tokenize_line(int line, int *out_triplets, int max_triplets);
int lc_editor_undo(void);
int lc_editor_redo(void);
}

static void *projectHandle = nullptr;

static std::string fromJava(JNIEnv *env, jstring value) {
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    std::string result = bytes ? bytes : "";
    if (bytes) env->ReleaseStringUTFChars(value, bytes);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_MainActivity_nativeVersion(JNIEnv *env, jclass) {
    return env->NewStringUTF(luac_version());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_MainActivity_nativeCheck(JNIEnv *env, jclass, jstring source) {
    std::string code = fromJava(env, source);
    char *error = nullptr;
    int valid = luac_check(code.c_str(), "AndroidEditor.lua", &error);
    std::string result = valid ? "Syntax OK\nNative machine-code JIT ready; tri-color incremental GC, classes/structs, and direct graphics." : error;
    luac_free(error);
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_MainActivity_nativeGenerate(JNIEnv *env, jclass, jstring source) {
    std::string code = fromJava(env, source);
    char *error = nullptr;
    char *generated = luac_compile_c(code.c_str(), "AndroidEditor.lua", &error);
    std::string result = generated ? generated : (error ? error : "Code generation failed");
    luac_free(generated);
    luac_free(error);
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_MainActivity_nativeOpenProject(JNIEnv *env, jclass, jstring root) {
    std::string path = fromJava(env, root);
    char *error = nullptr;
    if (projectHandle) { luac_project_close(projectHandle); projectHandle = nullptr; }
    projectHandle = luac_project_open(path.c_str(), &error);
    if (!projectHandle) {
        std::string message = error ? error : "Cannot open project";
        luac_free(error);
        return env->NewStringUTF(message.c_str());
    }
    char *frame = luac_project_step(projectHandle, 0);
    jstring result = env->NewStringUTF(frame ? frame : "{\"ok\":false,\"error\":\"runtime failure\"}");
    luac_free(frame);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_MainActivity_nativeStepProject(JNIEnv *env, jclass, jdouble dt) {
    if (!projectHandle) return env->NewStringUTF("{\"ok\":false,\"error\":\"project is not running\",\"output\":\"\",\"entities\":[],\"texts\":[]}");
    char *frame = luac_project_step(projectHandle, dt);
    jstring result = env->NewStringUTF(frame ? frame : "{\"ok\":false,\"error\":\"runtime failure\"}");
    luac_free(frame);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_MainActivity_nativeSetViewport(JNIEnv *, jclass, jint width, jint height) {
    if (projectHandle) luac_project_set_viewport(projectHandle, width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_MainActivity_nativeTouchProject(JNIEnv *, jclass, jint id, jdouble x, jdouble y, jboolean down) {
    if (projectHandle) luac_project_touch(projectHandle, id, x, y, down == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_MainActivity_nativeCloseProject(JNIEnv *, jclass) {
    if (projectHandle) luac_project_close(projectHandle);
    projectHandle = nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_luac_studio_MainActivity_nativeVulkanCreate(JNIEnv *env, jclass, jobject surface) {
    return androidVulkanCreate(env, surface) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_MainActivity_nativeVulkanDraw(JNIEnv *, jclass) { androidVulkanDraw(); }
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_MainActivity_nativeVulkanDestroy(JNIEnv *, jclass) { androidVulkanDestroy(); }

extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeSetText(JNIEnv *env, jclass, jstring text) {
    lc_editor_set_text(fromJava(env, text).c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_CodeEditorView_nativeGetText(JNIEnv *env, jclass) {
    return env->NewStringUTF(lc_editor_get_text());
}
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeInsert(JNIEnv *env, jclass, jstring text) {
    lc_editor_insert(fromJava(env, text).c_str());
}
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeNewline(JNIEnv *, jclass) { lc_editor_newline(); }
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeBackspace(JNIEnv *, jclass) { lc_editor_backspace(); }
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeDelete(JNIEnv *, jclass) { lc_editor_delete(); }
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeSetCursor(JNIEnv *, jclass, jint line, jint col) {
    lc_editor_set_cursor_line_col(line, col);
}
extern "C" JNIEXPORT void JNICALL
Java_com_luac_studio_CodeEditorView_nativeMoveCursor(JNIEnv *, jclass, jint dx, jint dy) {
    lc_editor_move_cursor(dx, dy);
}
extern "C" JNIEXPORT jint JNICALL
Java_com_luac_studio_CodeEditorView_nativeLineCount(JNIEnv *, jclass) { return lc_editor_line_count(); }
extern "C" JNIEXPORT jint JNICALL
Java_com_luac_studio_CodeEditorView_nativeCursorLine(JNIEnv *, jclass) { return lc_editor_cursor_line(); }
extern "C" JNIEXPORT jint JNICALL
Java_com_luac_studio_CodeEditorView_nativeCursorCol(JNIEnv *, jclass) { return lc_editor_cursor_col(); }
extern "C" JNIEXPORT jstring JNICALL
Java_com_luac_studio_CodeEditorView_nativeGetLine(JNIEnv *env, jclass, jint line) {
    char buf[4096];
    lc_editor_get_line(line, buf, sizeof(buf));
    return env->NewStringUTF(buf);
}
extern "C" JNIEXPORT jintArray JNICALL
Java_com_luac_studio_CodeEditorView_nativeTokenizeLine(JNIEnv *env, jclass, jint line) {
    int raw[512 * 3];
    int count = lc_editor_tokenize_line(line, raw, 512);
    jintArray arr = env->NewIntArray(count * 3);
    if (arr && count > 0) env->SetIntArrayRegion(arr, 0, count * 3, raw);
    return arr;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_luac_studio_CodeEditorView_nativeUndo(JNIEnv *, jclass) { return lc_editor_undo() ? JNI_TRUE : JNI_FALSE; }
extern "C" JNIEXPORT jboolean JNICALL
Java_com_luac_studio_CodeEditorView_nativeRedo(JNIEnv *, jclass) { return lc_editor_redo() ? JNI_TRUE : JNI_FALSE; }
