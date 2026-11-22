#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "obsidian_core.h"
#include "studio_app.hpp"

extern bool androidVulkanCreate(JNIEnv *env, jobject surface, int width, int height);
extern void androidVulkanResize(int width, int height);
extern void androidVulkanDrawStudio(StudioApp *app, float density);
extern void androidVulkanDestroy();

namespace {
JavaVM *g_vm = nullptr;
jobject g_activity = nullptr;
std::unique_ptr<StudioApp> g_studio;
std::mutex g_studioMutex;
float g_density = 3.0f;

std::string fromJava(JNIEnv *env, jstring value) {
    if (!value) return "";
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    std::string result = bytes ? bytes : "";
    if (bytes) env->ReleaseStringUTFChars(value, bytes);
    return result;
}

JNIEnv *getEnv() {
    if (!g_vm) return nullptr;
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return nullptr;
}
} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioInit(
    JNIEnv *env, jobject thiz, jstring projectsRoot, jstring initialProj, jboolean is3D,
    jfloat density, jint fam, jint sz, jint theme, jint tabs, jboolean lineNums, jstring customFont) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(thiz);
    g_density = density > 0.5f ? density : 3.0f;

    g_studio = std::make_unique<StudioApp>();
    g_studio->callbacks.setKeyboardVisible = [](bool show) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbSetKeyboard", "(Z)V");
        if (mid) e->CallVoidMethod(g_activity, mid, show ? JNI_TRUE : JNI_FALSE);
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.openFontPicker = []() {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbOpenFontPicker", "()V");
        if (mid) e->CallVoidMethod(g_activity, mid);
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.openTexturePicker = [](const std::string &projName, const std::string &nodeId) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbOpenTexturePicker", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (mid) {
            jstring jp = e->NewStringUTF(projName.c_str());
            jstring jn = e->NewStringUTF(nodeId.c_str());
            e->CallVoidMethod(g_activity, mid, jp, jn);
            e->DeleteLocalRef(jp);
            e->DeleteLocalRef(jn);
        }
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.openModelPicker = [](const std::string &projName, const std::string &nodeId) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbOpenModelPicker", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (mid) {
            jstring jp = e->NewStringUTF(projName.c_str());
            jstring jn = e->NewStringUTF(nodeId.c_str());
            e->CallVoidMethod(g_activity, mid, jp, jn);
            e->DeleteLocalRef(jp);
            e->DeleteLocalRef(jn);
        }
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.buildApk = [](const std::string &projName, const std::string &appName, bool is3D) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbBuildApk", "(Ljava/lang/String;Ljava/lang/String;Z)V");
        if (mid) {
            jstring jp = e->NewStringUTF(projName.c_str());
            jstring ja = e->NewStringUTF(appName.c_str());
            e->CallVoidMethod(g_activity, mid, jp, ja, is3D ? JNI_TRUE : JNI_FALSE);
            e->DeleteLocalRef(jp);
            e->DeleteLocalRef(ja);
        }
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.setOrientationPortrait = [](bool portrait) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbSetOrientation", "(Z)V");
        if (mid) e->CallVoidMethod(g_activity, mid, portrait ? JNI_TRUE : JNI_FALSE);
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.createProject = [](const std::string &kind, const std::string &name) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbCreateProject", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (mid) {
            jstring jk = e->NewStringUTF(kind.c_str());
            jstring jn = e->NewStringUTF(name.c_str());
            e->CallVoidMethod(g_activity, mid, jk, jn);
            e->DeleteLocalRef(jk);
            e->DeleteLocalRef(jn);
        }
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.renameProject = [](const std::string &oldName, const std::string &newName) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbRenameProject", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (mid) {
            jstring jo = e->NewStringUTF(oldName.c_str());
            jstring jn = e->NewStringUTF(newName.c_str());
            e->CallVoidMethod(g_activity, mid, jo, jn);
            e->DeleteLocalRef(jo);
            e->DeleteLocalRef(jn);
        }
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.deleteProject = [](const std::string &name) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbDeleteProject", "(Ljava/lang/String;)V");
        if (mid) {
            jstring jn = e->NewStringUTF(name.c_str());
            e->CallVoidMethod(g_activity, mid, jn);
            e->DeleteLocalRef(jn);
        }
        e->DeleteLocalRef(cls);
    };
    g_studio->callbacks.listProjects = []() -> std::vector<std::pair<std::string, bool>> {
        std::vector<std::pair<std::string, bool>> res;
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return res;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbListProjects", "()[Ljava/lang/String;");
        if (mid) {
            auto arr = (jobjectArray)e->CallObjectMethod(g_activity, mid);
            if (arr) {
                jsize n = e->GetArrayLength(arr);
                for (jsize i = 0; i < n; ++i) {
                    auto js = (jstring)e->GetObjectArrayElement(arr, i);
                    std::string s = fromJava(e, js);
                    e->DeleteLocalRef(js);
                    auto bar = s.rfind('|');
                    if (bar != std::string::npos) {
                        res.emplace_back(s.substr(0, bar), s.substr(bar + 1) == "3d");
                    }
                }
                e->DeleteLocalRef(arr);
            }
        }
        e->DeleteLocalRef(cls);
        return res;
    };
    g_studio->callbacks.savePrefs = [](int f, int s, int th, int tb, bool ln, const std::string &cf) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbSavePrefs", "(IIIIZLjava/lang/String;)V");
        if (mid) {
            jstring jcf = e->NewStringUTF(cf.c_str());
            e->CallVoidMethod(g_activity, mid, f, s, th, tb, ln ? JNI_TRUE : JNI_FALSE, jcf);
            e->DeleteLocalRef(jcf);
        }
        e->DeleteLocalRef(cls);
    };

    g_studio->init(fromJava(env, projectsRoot), fromJava(env, initialProj), is3D == JNI_TRUE,
                   fam, sz, theme, tabs, lineNums == JNI_TRUE, fromJava(env, customFont));
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioStep(JNIEnv *, jclass, jfloat dt) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) g_studio->step(dt);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioTouch(
    JNIEnv *env, jclass, jint action, jint pointerIndex, jintArray jIds, jfloatArray jXs, jfloatArray jYs) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (!g_studio || !jIds || !jXs || !jYs) return;
    jsize n = env->GetArrayLength(jIds);
    jint *ids = env->GetIntArrayElements(jIds, nullptr);
    jfloat *xs = env->GetFloatArrayElements(jXs, nullptr);
    jfloat *ys = env->GetFloatArrayElements(jYs, nullptr);
    g_studio->onTouch(action, pointerIndex, (int)n, ids, xs, ys);
    env->ReleaseIntArrayElements(jIds, ids, JNI_ABORT);
    env->ReleaseFloatArrayElements(jXs, xs, JNI_ABORT);
    env->ReleaseFloatArrayElements(jYs, ys, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioTextInput(JNIEnv *env, jclass, jstring utf8) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) g_studio->onTextInput(fromJava(env, utf8));
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioKeyCode(JNIEnv *, jclass, jint keyCode) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) g_studio->onKeyCode(keyCode);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioSetCustomFont(JNIEnv *env, jclass, jstring path) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) {
        std::string p = fromJava(env, path);
        g_studio->applyFont(p, g_studio->prefFontFamily);
        if (g_studio->callbacks.savePrefs) {
            g_studio->callbacks.savePrefs(g_studio->prefFontFamily, g_studio->prefFontSize,
                                          g_studio->prefTheme, g_studio->prefTabSpaces,
                                          g_studio->prefLineNumbers, p);
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioApplyTexture(JNIEnv *env, jclass, jstring jNodeId, jstring jRelPath) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) {
        g_studio->applyNodeTexture(fromJava(env, jNodeId), fromJava(env, jRelPath));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioApplyModel(JNIEnv *env, jclass, jstring jNodeId, jstring jRelPath) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) {
        g_studio->applyNodeModel(fromJava(env, jNodeId), fromJava(env, jRelPath));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioDestroy(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) {
        g_studio->dispose();
        g_studio.reset();
    }
    if (g_activity) {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeVulkanCreate(JNIEnv *env, jclass, jobject surface, jint width, jint height) {
    return androidVulkanCreate(env, surface, width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeVulkanResize(JNIEnv *, jclass, jint width, jint height) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    androidVulkanResize(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeVulkanDraw(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    androidVulkanDrawStudio(g_studio.get(), g_density);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeVulkanDestroy(JNIEnv *, jclass) {
    androidVulkanDestroy();
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_MainActivity_nativeStudioApkResult(JNIEnv *env, jclass, jboolean ok, jstring jMsg) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) g_studio->onApkBuildResult(ok == JNI_TRUE, fromJava(env, jMsg));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_obsidiancore_studio_ApkBuilder_nativeCompileCheckObs(JNIEnv *env, jclass, jstring jSource, jstring jFilename) {
    std::string src = fromJava(env, jSource);
    std::string fn = fromJava(env, jFilename);
    char *err = nullptr;
    char *cCode = obsidian_compile_c(src.c_str(), fn.c_str(), &err);
    if (!cCode) {
        std::string msg = err ? err : "Obsidronium compile error";
        if (err) obsidian_free(err);
        return env->NewStringUTF(msg.c_str());
    }
    obsidian_free(cCode);
    if (err) obsidian_free(err);
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativePlayerInit(
    JNIEnv *env, jobject thiz, jstring projectsRoot, jstring projName, jboolean is3D, jfloat density) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(thiz);
    g_density = density > 0.5f ? density : 3.0f;

    g_studio = std::make_unique<StudioApp>();
    g_studio->callbacks.setOrientationPortrait = [](bool portrait) {
        JNIEnv *e = getEnv();
        if (!e || !g_activity) return;
        jclass cls = e->GetObjectClass(g_activity);
        jmethodID mid = e->GetMethodID(cls, "nativeCbSetOrientation", "(Z)V");
        if (mid) e->CallVoidMethod(g_activity, mid, portrait ? JNI_TRUE : JNI_FALSE);
        e->DeleteLocalRef(cls);
    };
    g_studio->initPlayer(fromJava(env, projectsRoot), fromJava(env, projName), is3D == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativePlayerStep(JNIEnv *, jclass, jfloat dt) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) g_studio->step(dt);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativePlayerTouch(
    JNIEnv *env, jclass, jint action, jint pointerIndex, jintArray jIds, jfloatArray jXs, jfloatArray jYs) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (!g_studio || !jIds || !jXs || !jYs) return;
    jsize n = env->GetArrayLength(jIds);
    jint *ids = env->GetIntArrayElements(jIds, nullptr);
    jfloat *xs = env->GetFloatArrayElements(jXs, nullptr);
    jfloat *ys = env->GetFloatArrayElements(jYs, nullptr);
    g_studio->onTouch(action, pointerIndex, (int)n, ids, xs, ys);
    env->ReleaseIntArrayElements(jIds, ids, JNI_ABORT);
    env->ReleaseFloatArrayElements(jXs, xs, JNI_ABORT);
    env->ReleaseFloatArrayElements(jYs, ys, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativePlayerDestroy(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    if (g_studio) {
        g_studio->dispose();
        g_studio.reset();
    }
    if (g_activity) {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativeVulkanCreate(JNIEnv *env, jclass, jobject surface, jint width, jint height) {
    return androidVulkanCreate(env, surface, width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativeVulkanResize(JNIEnv *, jclass, jint width, jint height) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    androidVulkanResize(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativeVulkanDraw(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lk(g_studioMutex);
    androidVulkanDrawStudio(g_studio.get(), g_density);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_GamePlayerActivity_nativeVulkanDestroy(JNIEnv *, jclass) {
    androidVulkanDestroy();
}
