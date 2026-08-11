#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "reimpl/audio.h"
#include "utils/logger.h"

extern void port_trace(const char *format, ...);

/*
 * Java callbacks requested by Gameloft's Sandstorm 2 engine.  Rendering,
 * input and game simulation remain native.  The old Android MediaPlayer,
 * SoundPool, online and playlist services are deliberately harmless during
 * the first bring-up; every call is represented so JNI lookup can continue.
 */

static int dummy_java_object;
static unsigned java_stub_calls;

static void trace_stub(jmethodID id, const char *kind) {
    unsigned call = ++java_stub_calls;
    if (call <= 80 || call % 500 == 0)
        port_trace("Java: %s stub id=%d call=%u", kind, (int)id, call);
}

static void method_void_stub(jmethodID id, va_list args) {
    (void)args;
    trace_stub(id, "void");
}

static void method_exit(jmethodID id, va_list args) {
    (void)args;
    port_trace("Java: game requested Exit/sendAppToBackground id=%d",
               (int)id);
}

static const char *java_string_chars(jstring value) {
    return value ? jni->GetStringUTFChars(&jni, value, NULL) : NULL;
}

static void java_string_release(jstring value, const char *chars) {
    if (value && chars)
        jni->ReleaseStringUTFChars(&jni, value, chars);
}

static void method_audio_load_music(jmethodID id, va_list args) {
    (void)id;
    jint sound_id = va_arg(args, jint);
    jstring value = va_arg(args, jstring);
    const char *name = java_string_chars(value);
    mc2_audio_load_music(sound_id, name);
    java_string_release(value, name);
}

static void method_audio_load_sound(jmethodID id, va_list args) {
    (void)id;
    jint sound_id = va_arg(args, jint);
    jstring value = va_arg(args, jstring);
    const char *name = java_string_chars(value);
    mc2_audio_load_sound(sound_id, name);
    java_string_release(value, name);
}

static void method_audio_play_music(jmethodID id, va_list args) {
    (void)id;
    jint sound_id = va_arg(args, jint);
    jint loop = va_arg(args, jint);
    mc2_audio_play_music(sound_id, loop);
}

static void method_audio_play_sound(jmethodID id, va_list args) {
    (void)id;
    jint sound_id = va_arg(args, jint);
    jint loop = va_arg(args, jint);
    jstring value = va_arg(args, jstring);
    double volume = va_arg(args, double);
    const char *name = java_string_chars(value);
    mc2_audio_play_sound(sound_id, loop, name, (float)volume);
    java_string_release(value, name);
}

static void method_audio_pause_music(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_pause_music(va_arg(args, jint));
}

static void method_audio_resume_music(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_resume_music(va_arg(args, jint));
}

static void method_audio_stop_music(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_stop_music(va_arg(args, jint));
}

static void method_audio_pause_sound(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_pause_sound(va_arg(args, jint));
}

static void method_audio_resume_sound(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_resume_sound(va_arg(args, jint));
}

static void method_audio_stop_sound(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_stop_sound(va_arg(args, jint));
}

static void method_audio_reset_sound(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_reset_sound(va_arg(args, jint));
}

static void method_audio_set_volume(jmethodID id, va_list args) {
    (void)id;
    jint sound_id = va_arg(args, jint);
    double volume = va_arg(args, double);
    mc2_audio_set_volume(sound_id, (float)volume);
}

static void method_audio_unload_music(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_unload_music(va_arg(args, jint));
}

static void method_audio_unload_sound(jmethodID id, va_list args) {
    (void)id;
    mc2_audio_unload_sound(va_arg(args, jint));
}

static void method_audio_pause_all(jmethodID id, va_list args) {
    (void)id; (void)args;
    mc2_audio_pause_all();
}

static void method_audio_resume_all(jmethodID id, va_list args) {
    (void)id; (void)args;
    mc2_audio_resume_all();
}

static void method_audio_stop_all(jmethodID id, va_list args) {
    (void)id; (void)args;
    mc2_audio_stop_all();
}

static void method_audio_stop_all_sounds(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    mc2_audio_stop_sound(-1);
}

static jint integer_sound_loaded(jmethodID id, va_list args) {
    (void)id;
    return mc2_audio_is_sound_loaded(va_arg(args, jint));
}

static jint integer_music_playing(jmethodID id, va_list args) {
    (void)id;
    return mc2_audio_is_music_playing(va_arg(args, jint));
}

static jint integer_media_playing(jmethodID id, va_list args) {
    (void)id;
    jint sound_id = va_arg(args, jint);
    return mc2_audio_is_music_playing(sound_id) ||
           mc2_audio_is_sound_playing(sound_id);
}

static jint integer_music_duration(jmethodID id, va_list args) {
    (void)id;
    return mc2_audio_get_music_duration(va_arg(args, jint));
}

static jint integer_sound_duration(jmethodID id, va_list args) {
    (void)id;
    return mc2_audio_get_sound_duration(va_arg(args, jint));
}

static jint integer_sound_status(jmethodID id, va_list args) {
    (void)id;
    return mc2_audio_is_sound_playing(va_arg(args, jint));
}

static jobject object_dummy(jmethodID id, va_list args) {
    (void)args;
    trace_stub(id, "object");
    return (jobject)&dummy_java_object;
}

static jobject string_empty(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "");
}

static jobject string_data_path(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni,
        "/sdcard/gameloft/games/GloftBPHP/");
}

static jobject string_package(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni,
        "com.gameloft.android.GAND.GloftBPHP.ML");
}

static jobject string_country(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "US");
}

static jboolean boolean_false(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jboolean boolean_true(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_TRUE;
}

static jint integer_zero(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jint integer_one(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 1;
}

static jint integer_width(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 960;
}

static jint integer_height(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 544;
}

static jint integer_manufacturer_sony(jmethodID id, va_list args) {
    (void)id; (void)args;
    /* Game.getManufacture() returns profile 4 for the Sony/Xperia branch. */
    return 4;
}

static jlong current_time_millis(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jlong)(sceKernelGetProcessTimeWide() / 1000);
}

enum {
    MID_GENERIC_VOID = 1,
    MID_EXIT,
    MID_GET_CONTEXT,
    MID_GET_INSTANCE,
    MID_GET_PATH,
    MID_GET_ABSOLUTE_PATH,
    MID_GET_PACKAGE_NAME,
    MID_GET_COUNTRY,
    MID_STRING_EMPTY,
    MID_READ_FILE,
    MID_GET_PLAYLIST_NAME,
    MID_BOOLEAN_FALSE,
    MID_BOOLEAN_TRUE,
    MID_WRITE_FILE,
    MID_IS_SOUND_LOADED,
    MID_INTEGER_ZERO,
    MID_GET_WIDTH,
    MID_GET_HEIGHT,
    MID_GET_MANUFACTURE,
    MID_CURRENT_TIME,
    MID_AUDIO_LOAD_MUSIC,
    MID_AUDIO_LOAD_SOUND,
    MID_AUDIO_PLAY_MUSIC,
    MID_AUDIO_PLAY_SOUND,
    MID_AUDIO_PAUSE_MUSIC,
    MID_AUDIO_RESUME_MUSIC,
    MID_AUDIO_STOP_MUSIC,
    MID_AUDIO_PAUSE_SOUND,
    MID_AUDIO_RESUME_SOUND,
    MID_AUDIO_STOP_SOUND,
    MID_AUDIO_RESET_SOUND,
    MID_AUDIO_SET_VOLUME,
    MID_AUDIO_UNLOAD_MUSIC,
    MID_AUDIO_UNLOAD_SOUND,
    MID_AUDIO_PAUSE_ALL,
    MID_AUDIO_RESUME_ALL,
    MID_AUDIO_STOP_ALL,
    MID_AUDIO_STOP_ALL_SOUNDS,
    MID_AUDIO_IS_SOUND_LOADED,
    MID_AUDIO_IS_MEDIA_PLAYING,
    MID_AUDIO_IS_MUSIC_PLAYING,
    MID_AUDIO_MUSIC_DURATION,
    MID_AUDIO_SOUND_DURATION,
    MID_AUDIO_SOUND_STATUS,
};

NameToMethodID nameToMethodId[] = {
    { MID_GET_CONTEXT, "getContext", METHOD_TYPE_OBJECT },
    { MID_GET_CONTEXT, "getResources", METHOD_TYPE_OBJECT },
    { MID_GET_CONTEXT, "getDefault", METHOD_TYPE_OBJECT },
    { MID_GET_CONTEXT, "getByName", METHOD_TYPE_OBJECT },
    { MID_GET_CONTEXT, "getParent", METHOD_TYPE_OBJECT },
    { MID_GET_CONTEXT, "getSource", METHOD_TYPE_OBJECT },
    { MID_GET_INSTANCE, "getInstance", METHOD_TYPE_OBJECT },
    { MID_GET_PATH, "getPath", METHOD_TYPE_OBJECT },
    { MID_GET_ABSOLUTE_PATH, "getAbsolutePath", METHOD_TYPE_OBJECT },
    { MID_GET_PACKAGE_NAME, "getPackageName", METHOD_TYPE_OBJECT },
    { MID_GET_COUNTRY, "getCountry", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "UserName", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "Password", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "getMessage", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "getName", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "getString", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "getText", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "getValue", METHOD_TYPE_OBJECT },
    { MID_STRING_EMPTY, "toString", METHOD_TYPE_OBJECT },
    { MID_READ_FILE, "ReadFile", METHOD_TYPE_OBJECT },
    { MID_GET_PLAYLIST_NAME, "GetPlayListName", METHOD_TYPE_OBJECT },

    { MID_WRITE_FILE, "WriteFile", METHOD_TYPE_BOOLEAN },
    { MID_BOOLEAN_FALSE, "IsWifiEnable", METHOD_TYPE_BOOLEAN },
    { MID_BOOLEAN_FALSE, "exists", METHOD_TYPE_BOOLEAN },
    { MID_BOOLEAN_FALSE, "isConnected", METHOD_TYPE_BOOLEAN },
    { MID_BOOLEAN_FALSE, "isDirectory", METHOD_TYPE_BOOLEAN },
    { MID_BOOLEAN_FALSE, "isEmpty", METHOD_TYPE_BOOLEAN },
    { MID_BOOLEAN_TRUE, "ready", METHOD_TYPE_BOOLEAN },

    { MID_AUDIO_IS_SOUND_LOADED, "isSoundLoaded", METHOD_TYPE_INT },
    { MID_AUDIO_IS_MEDIA_PLAYING, "isMediaPlaying", METHOD_TYPE_INT },
    { MID_AUDIO_IS_MUSIC_PLAYING, "isMusicPlaying", METHOD_TYPE_INT },
    { MID_AUDIO_MUSIC_DURATION, "getMusicDuration", METHOD_TYPE_INT },
    { MID_AUDIO_SOUND_DURATION, "getSoundDuration", METHOD_TYPE_INT },
    { MID_AUDIO_SOUND_STATUS, "getSoundStatus", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "GetPhoneLanguage", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "GetNumPlaylists", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "DisablePlaylist", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "isWifiEnabled", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "available", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "getCount", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "getId", METHOD_TYPE_INT },
    { MID_INTEGER_ZERO, "length", METHOD_TYPE_INT },
    { MID_GET_WIDTH, "getWidth", METHOD_TYPE_INT },
    { MID_GET_HEIGHT, "getHeight", METHOD_TYPE_INT },
    { MID_GET_MANUFACTURE, "getManufacture", METHOD_TYPE_INT },

    { MID_CURRENT_TIME, "GetCurrentTime", METHOD_TYPE_LONG },

    { MID_EXIT, "Exit", METHOD_TYPE_VOID },
    { MID_EXIT, "sendAppToBackground", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "ChangeMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_ALL, "Pause", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_ALL, "PauseMusicBG", METHOD_TYPE_VOID },
    { MID_AUDIO_RESUME_ALL, "PlayBGMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_RESUME_ALL, "ResumeMusicBG", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "SetPlaylist", METHOD_TYPE_VOID },
    { MID_AUDIO_STOP_ALL, "StopMusicBG", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "launchGLLive", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "launchIGP", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "notifyTrophy", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "openBrowser", METHOD_TYPE_VOID },
    { MID_AUDIO_LOAD_MUSIC, "loadMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_LOAD_SOUND, "loadSound", METHOD_TYPE_VOID },
    { MID_AUDIO_LOAD_SOUND, "loadSoundPool", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_ALL, "pauseAllMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_ALL, "pauseAllSound", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_ALL, "pauseAllSounds", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_MUSIC, "pauseMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_PAUSE_SOUND, "pauseSound", METHOD_TYPE_VOID },
    { MID_AUDIO_PLAY_MUSIC, "playMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_PLAY_SOUND, "playSound", METHOD_TYPE_VOID },
    { MID_AUDIO_RESET_SOUND, "resetSound", METHOD_TYPE_VOID },
    { MID_AUDIO_RESUME_ALL, "resumeAllSound", METHOD_TYPE_VOID },
    { MID_AUDIO_RESUME_MUSIC, "resumeMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_RESUME_SOUND, "resumeSound", METHOD_TYPE_VOID },
    { MID_AUDIO_SET_VOLUME, "setVolume", METHOD_TYPE_VOID },
    { MID_AUDIO_STOP_ALL, "stopAllMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_STOP_ALL_SOUNDS, "stopAllSounds", METHOD_TYPE_VOID },
    { MID_AUDIO_STOP_MUSIC, "stopMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_STOP_SOUND, "stopSound", METHOD_TYPE_VOID },
    { MID_AUDIO_UNLOAD_MUSIC, "unloadMusic", METHOD_TYPE_VOID },
    { MID_AUDIO_UNLOAD_SOUND, "unloadSound", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "close", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "connect", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "disconnect", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "flush", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "release", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "start", METHOD_TYPE_VOID },
    { MID_GENERIC_VOID, "stop", METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {
    { MID_BOOLEAN_FALSE, boolean_false },
    { MID_BOOLEAN_TRUE, boolean_true },
    { MID_WRITE_FILE, boolean_true },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { MID_AUDIO_IS_SOUND_LOADED, integer_sound_loaded },
    { MID_AUDIO_IS_MEDIA_PLAYING, integer_media_playing },
    { MID_AUDIO_IS_MUSIC_PLAYING, integer_music_playing },
    { MID_AUDIO_MUSIC_DURATION, integer_music_duration },
    { MID_AUDIO_SOUND_DURATION, integer_sound_duration },
    { MID_AUDIO_SOUND_STATUS, integer_sound_status },
    { MID_INTEGER_ZERO, integer_zero },
    { MID_GET_WIDTH, integer_width },
    { MID_GET_HEIGHT, integer_height },
    { MID_GET_MANUFACTURE, integer_manufacturer_sony },
};
MethodsLong methodsLong[] = {
    { MID_CURRENT_TIME, current_time_millis },
};
MethodsObject methodsObject[] = {
    { MID_GET_CONTEXT, object_dummy },
    { MID_GET_INSTANCE, object_dummy },
    { MID_GET_PATH, string_data_path },
    { MID_GET_ABSOLUTE_PATH, string_data_path },
    { MID_GET_PACKAGE_NAME, string_package },
    { MID_GET_COUNTRY, string_country },
    { MID_STRING_EMPTY, string_empty },
    { MID_READ_FILE, string_empty },
    { MID_GET_PLAYLIST_NAME, string_empty },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { MID_GENERIC_VOID, method_void_stub },
    { MID_EXIT, method_exit },
    { MID_AUDIO_LOAD_MUSIC, method_audio_load_music },
    { MID_AUDIO_LOAD_SOUND, method_audio_load_sound },
    { MID_AUDIO_PLAY_MUSIC, method_audio_play_music },
    { MID_AUDIO_PLAY_SOUND, method_audio_play_sound },
    { MID_AUDIO_PAUSE_MUSIC, method_audio_pause_music },
    { MID_AUDIO_RESUME_MUSIC, method_audio_resume_music },
    { MID_AUDIO_STOP_MUSIC, method_audio_stop_music },
    { MID_AUDIO_PAUSE_SOUND, method_audio_pause_sound },
    { MID_AUDIO_RESUME_SOUND, method_audio_resume_sound },
    { MID_AUDIO_STOP_SOUND, method_audio_stop_sound },
    { MID_AUDIO_RESET_SOUND, method_audio_reset_sound },
    { MID_AUDIO_SET_VOLUME, method_audio_set_volume },
    { MID_AUDIO_UNLOAD_MUSIC, method_audio_unload_music },
    { MID_AUDIO_UNLOAD_SOUND, method_audio_unload_sound },
    { MID_AUDIO_PAUSE_ALL, method_audio_pause_all },
    { MID_AUDIO_RESUME_ALL, method_audio_resume_all },
    { MID_AUDIO_STOP_ALL, method_audio_stop_all },
    { MID_AUDIO_STOP_ALL_SOUNDS, method_audio_stop_all_sounds },
};

const int SDK_INT = 10;
char WINDOW_SERVICE[] = "window";

NameToFieldID nameToFieldId[] = {
    { 1, "SDK_INT", FIELD_TYPE_INT },
    { 2, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    { 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
    { 2, (jobject)WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
