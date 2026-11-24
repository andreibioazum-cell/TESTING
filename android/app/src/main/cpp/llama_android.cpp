#include <jni.h>
#include <android/log.h>
#include "llama.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ObsidianAI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ObsidianAI", __VA_ARGS__)

namespace {
std::mutex g_mutex;
llama_model *g_model = nullptr;
std::string g_loadedPath;
int g_nCtx = 4096;
int g_nThreads = 4;
std::atomic<bool> g_abort{false};
std::atomic<bool> g_backendInited{false};

std::string fromJString(JNIEnv *env, jstring js) {
    if (!js) return {};
    const char *chars = env->GetStringUTFChars(js, nullptr);
    if (!chars) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(js, chars);
    return out;
}

void ensureBackend() {
    if (!g_backendInited.exchange(true)) {
        llama_backend_init();
    }
}

void batchSetTokens(llama_batch_ext *batch, const llama_token *tokens, int32_t n_tokens, llama_pos pos0, bool logitsLast) {
    llama_batch_ext_clear(batch);
    for (int32_t i = 0; i < n_tokens; ++i) {
        const int32_t idx = llama_batch_ext_add_token(batch, 0, tokens[i]);
        const llama_pos pos = pos0 + i;
        llama_batch_ext_set_pos(batch, idx, &pos);
    }
    if (logitsLast && n_tokens > 0) {
        llama_batch_ext_set_output_logits(batch, n_tokens - 1, true);
    }
}

std::string formatChatPrompt(const llama_model *model, const std::vector<std::string> &roles, const std::vector<std::string> &contents) {
    std::vector<llama_chat_message> msgs;
    msgs.reserve(roles.size());
    size_t totalChars = 0;
    for (size_t i = 0; i < roles.size(); ++i) {
        msgs.push_back({roles[i].c_str(), contents[i].c_str()});
        totalChars += roles[i].size() + contents[i].size() + 32;
    }
    const char *tmpl = llama_model_chat_template(model, nullptr);
    if (tmpl && *tmpl) {
        std::vector<char> buf(std::max<size_t>(2048, totalChars * 2 + 512));
        int32_t n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), (int32_t)buf.size());
        if (n > (int32_t)buf.size()) {
            buf.resize((size_t)n + 16);
            n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), (int32_t)buf.size());
        }
        if (n > 0) {
            return std::string(buf.data(), (size_t)n);
        }
    }
    // Fallback ChatML format
    std::string out;
    out.reserve(totalChars + 64);
    for (size_t i = 0; i < roles.size(); ++i) {
        out += "<|im_start|>";
        out += roles[i];
        out += "\n";
        out += contents[i];
        out += "<|im_end|>\n";
    }
    out += "<|im_start|>assistant\n";
    return out;
}

size_t utf8ValidPrefixLen(const std::string &s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need = 1;
        if (c < 0x80) need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else { i++; continue; }
        if (i + need > n) break;
        i += need;
    }
    return i;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeLoadModel(
    JNIEnv *env, jclass, jstring jPath, jint nCtx, jint nThreads) {
    std::lock_guard<std::mutex> lk(g_mutex);
    ensureBackend();

    if (g_model) {
        llama_model_free(g_model);
        g_model = nullptr;
        g_loadedPath.clear();
    }

    std::string path = fromJString(env, jPath);
    if (path.empty()) return env->NewStringUTF("ERROR: Empty model path");

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; // CPU arm64 NEON

    llama_model *m = llama_model_load_from_file(path.c_str(), mparams);
    if (!m) {
        return env->NewStringUTF(("ERROR: Failed to load GGUF model at " + path).c_str());
    }

    g_model = m;
    g_loadedPath = path;
    g_nCtx = std::max(512, std::min(16384, (int)nCtx));
    g_nThreads = std::max(1, std::min(12, (int)nThreads));

    char desc[256]{};
    llama_model_desc(m, desc, sizeof(desc));
    uint64_t params = llama_model_n_params(m);
    double bParams = params / 1e9;
    char info[512];
    snprintf(info, sizeof(info), "OK:%s (%.2fB params, ctx=%d, threads=%d)", desc, bParams, g_nCtx, g_nThreads);
    LOGI("Loaded model: %s", info);
    return env->NewStringUTF(info);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeUnloadModel(JNIEnv *, jclass) {
    g_abort.store(true);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_model) {
        llama_model_free(g_model);
        g_model = nullptr;
        g_loadedPath.clear();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeStopGeneration(JNIEnv *, jclass) {
    g_abort.store(true);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeIsLoaded(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_model != nullptr ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeGenerate(
    JNIEnv *env, jclass, jobjectArray jRoles, jobjectArray jContents,
    jint maxTokens, jfloat temp, jobject jCallback) {

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_model) return env->NewStringUTF("ERROR: No model loaded");
    g_abort.store(false);

    jsize msgCount = env->GetArrayLength(jRoles);
    std::vector<std::string> roles(msgCount), contents(msgCount);
    for (jsize i = 0; i < msgCount; ++i) {
        auto r = (jstring)env->GetObjectArrayElement(jRoles, i);
        auto c = (jstring)env->GetObjectArrayElement(jContents, i);
        roles[i] = fromJString(env, r);
        contents[i] = fromJString(env, c);
        if (r) env->DeleteLocalRef(r);
        if (c) env->DeleteLocalRef(c);
    }

    std::string prompt = formatChatPrompt(g_model, roles, contents);
    const llama_vocab *vocab = llama_model_get_vocab(g_model);

    int nPrompt = -llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(), nullptr, 0, true, true);
    if (nPrompt <= 0) return env->NewStringUTF("ERROR: Tokenization failed");
    std::vector<llama_token> tokens((size_t)nPrompt);
    if (llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(), tokens.data(), (int32_t)tokens.size(), true, true) < 0) {
        return env->NewStringUTF("ERROR: Tokenization failed");
    }

    int reserveGen = std::max(128, std::min((int)maxTokens, g_nCtx / 2));
    if ((int)tokens.size() > g_nCtx - reserveGen) {
        // Keep beginning + recent context within nCtx
        int keepTail = std::max(128, g_nCtx - reserveGen - 64);
        std::vector<llama_token> trimmed;
        trimmed.reserve(64 + keepTail);
        trimmed.insert(trimmed.end(), tokens.begin(), tokens.begin() + std::min<int>(64, (int)tokens.size()));
        trimmed.insert(trimmed.end(), tokens.end() - std::min<int>(keepTail, (int)tokens.size() - 64), tokens.end());
        tokens.swap(trimmed);
    }

    const int nBatch = 256;
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t)g_nCtx;
    cparams.n_batch = (uint32_t)nBatch;
    cparams.n_threads = g_nThreads;
    cparams.n_threads_batch = g_nThreads;
    cparams.no_perf = true;

    llama_context *ctx = llama_init_from_model(g_model, cparams);
    if (!ctx) return env->NewStringUTF("ERROR: Failed to allocate context (try smaller context or model)");

    auto sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
    llama_sampler *smpl = llama_sampler_chain_init(sparams);
    if (temp <= 0.02f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.92f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    }

    jclass cbCls = jCallback ? env->GetObjectClass(jCallback) : nullptr;
    jmethodID cbMid = cbCls ? env->GetMethodID(cbCls, "onToken", "(Ljava/lang/String;)V") : nullptr;

    llama_batch_ext *batch = llama_batch_ext_init(ctx);
    int nPos = 0;
    const int nPromptTokens = (int)tokens.size();
    bool failed = false;

    for (int off = 0; off < nPromptTokens; off += nBatch) {
        if (g_abort.load()) break;
        int chunk = std::min(nBatch, nPromptTokens - off);
        bool isLastChunk = (off + chunk == nPromptTokens);
        batchSetTokens(batch, tokens.data() + off, chunk, nPos, isLastChunk);
        if (llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0) {
            failed = true;
            break;
        }
        nPos += chunk;
    }

    std::string generated;
    std::string pendingUtf8;
    const int limit = std::min((int)maxTokens, std::max(64, g_nCtx - nPos));

    for (int step = 0; !failed && !g_abort.load() && step < limit; ++step) {
        llama_token tok = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;

        char piece[256];
        int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
        if (n > 0) {
            generated.append(piece, (size_t)n);
            pendingUtf8.append(piece, (size_t)n);
            size_t valid = utf8ValidPrefixLen(pendingUtf8);
            if (valid > 0 && jCallback && cbMid) {
                std::string chunkStr = pendingUtf8.substr(0, valid);
                pendingUtf8.erase(0, valid);
                jstring jChunk = env->NewStringUTF(chunkStr.c_str());
                env->CallVoidMethod(jCallback, cbMid, jChunk);
                env->DeleteLocalRef(jChunk);
            }
        }

        batchSetTokens(batch, &tok, 1, nPos, true);
        if (llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0) {
            failed = true;
            break;
        }
        nPos += 1;
    }

    if (!pendingUtf8.empty() && jCallback && cbMid) {
        size_t valid = utf8ValidPrefixLen(pendingUtf8);
        if (valid > 0) {
            jstring jChunk = env->NewStringUTF(pendingUtf8.substr(0, valid).c_str());
            env->CallVoidMethod(jCallback, cbMid, jChunk);
            env->DeleteLocalRef(jChunk);
        }
    }

    if (cbCls) env->DeleteLocalRef(cbCls);
    llama_batch_ext_free(batch);
    llama_sampler_free(smpl);
    llama_free(ctx);

    if (failed) return env->NewStringUTF("ERROR: Decode failed during generation");
    return env->NewStringUTF(generated.c_str());
}
