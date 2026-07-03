#ifndef __AUDIO_DECODER_H__
#define __AUDIO_DECODER_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_audio_dec.h"
#include "esp_aac_dec.h"
#include "esp_sbc_dec.h"
#include "esp_a2dp_api.h"
#include "BlockingQueue.h"
#include "AudioI2s.h"
#include "UiMusicPlayer.h"

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    void regAudioI2sHandle(AudioI2s* audio_i2s)   { _audio_i2s = audio_i2s;}
    void regUiMusicPlayerHandle(UiMusicPlayer* ui_music_player) { _ui_music_player = ui_music_player; }
    void start();
    void stop();
    esp_audio_err_t applyMcc(const esp_a2d_mcc_t *mcc);
    void decoderClose();
    void decoderDataFlush();
    void pushAudioData(esp_a2d_audio_buff_t *audio_buf);

private:
    static constexpr const char *_DECODER_TAG = "AudioDecoder";
    AudioI2s* _audio_i2s = nullptr;
    UiMusicPlayer* _ui_music_player = nullptr;

    SemaphoreHandle_t _audio_raw_data_mux = nullptr;
    esp_audio_dec_handle_t _dec_hdl = nullptr;
    std::atomic<bool> _dec_opened{false};

    std::thread _decode_task_thread;
    std::atomic<bool> _running{false};
    
    BlockingQueue <esp_a2d_audio_buff_t *> _a2dp_raw_queue;

    int getChannelCount(const esp_a2d_mcc_t *mcc);
    int getSampleRate(const esp_a2d_mcc_t *mcc);

    void audio_decode_task();
};

#endif // __AUDIO_DECODER_H__