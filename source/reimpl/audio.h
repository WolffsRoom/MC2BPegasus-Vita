#ifndef MC2_REIMPL_AUDIO_H
#define MC2_REIMPL_AUDIO_H

void mc2_audio_load_music(int id, const char *name);
void mc2_audio_load_sound(int id, const char *name);
void mc2_audio_play_music(int id, int loop);
void mc2_audio_play_sound(int id, int loop, const char *fallback_name,
                          float volume);
void mc2_audio_pause_music(int id);
void mc2_audio_resume_music(int id);
void mc2_audio_stop_music(int id);
void mc2_audio_pause_sound(int id);
void mc2_audio_resume_sound(int id);
void mc2_audio_stop_sound(int id);
void mc2_audio_pause_all(void);
void mc2_audio_resume_all(void);
void mc2_audio_stop_all(void);
void mc2_audio_set_volume(int id, float volume);
void mc2_audio_unload_music(int id);
void mc2_audio_unload_sound(int id);
void mc2_audio_reset_sound(int id);

int mc2_audio_is_sound_loaded(int id);
int mc2_audio_is_music_playing(int id);
int mc2_audio_is_sound_playing(int id);
int mc2_audio_get_music_duration(int id);
int mc2_audio_get_sound_duration(int id);
void mc2_audio_log_stats(void);

#endif
