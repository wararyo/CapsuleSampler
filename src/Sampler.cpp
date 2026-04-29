#include "Sampler.h"

#include <algorithm>
#include <Tables.h>
#include "Utils.h"

#if defined(FREERTOS)
// 長い処理ではセマフォを使用し、短い処理ではスピンロックを使用する
#define ENTER_CRITICAL_SPINLOCK(mutex) portENTER_CRITICAL(&mutex)
#define EXIT_CRITICAL_SPINLOCK(mutex) portEXIT_CRITICAL(&mutex)
#define ENTER_CRITICAL_SEMAPHORE(mutex) xSemaphoreTake(mutex, portMAX_DELAY)
#define EXIT_CRITICAL_SEMAPHORE(mutex) xSemaphoreGive(mutex)
#else
// 非FreeRTOS環境ではすべて同じ実装
#define ENTER_CRITICAL_SPINLOCK(mutex) mutex.lock()
#define EXIT_CRITICAL_SPINLOCK(mutex) mutex.unlock()
#define ENTER_CRITICAL_SEMAPHORE(mutex) mutex.lock()
#define EXIT_CRITICAL_SEMAPHORE(mutex) mutex.unlock()
#endif

using std::unique_ptr;
using std::shared_ptr;

namespace capsule
{
namespace sampler
{

void Sampler::InitializeMutexes() {
#if defined(FREERTOS)
    // 長時間の処理用のセマフォを初期化
    playersMutex = xSemaphoreCreateMutex();
    if (playersMutex == NULL) {
        // エラー処理（必要に応じて）
        LOGI("Sampler", "Failed to create playersMutex\n");
    }
#endif
}

#include <optional>

std::shared_ptr<const Sample> Timbre::GetAppropriateSample(uint8_t noteNo, uint8_t velocity)
{
    for (const auto& ms : *samples)
    {
        if (ms->lowerNoteNo <= noteNo && noteNo <= ms->upperNoteNo && ms->lowerVelocity <= velocity && velocity <= ms->upperVelocity)
            return ms->sample;
    }
    return nullptr;
}

void Sampler::SetTimbre(uint8_t channel, shared_ptr<Timbre> t)
{
    if(channel < CH_COUNT) channels[channel].SetTimbre(t);
}
void Sampler::Channel::SetTimbre(shared_ptr<Timbre> t)
{
    auto samplerPtr = sampler.lock();
    if (samplerPtr) {
        ENTER_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
        timbre = t;
        EXIT_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
    } else {
        timbre = t;
    }
}

void Sampler::NoteOn(uint8_t noteNo, uint8_t velocity, uint8_t channel)
{
    if (channel >= CH_COUNT) channel = 0; // 無効なチャンネルの場合は1CHにフォールバック
    velocity &= 0b01111111; // velocityを0-127の範囲に収める
    ENTER_CRITICAL_SPINLOCK(messageQueueMutex);
    messageQueue.push_back(Message{MessageStatus::NOTE_ON, channel, noteNo, velocity, 0});
    EXIT_CRITICAL_SPINLOCK(messageQueueMutex);
}
void Sampler::NoteOff(uint8_t noteNo, uint8_t velocity, uint8_t channel)
{
    if (channel >= CH_COUNT) channel = 0; // 無効なチャンネルの場合は1CHにフォールバック
    velocity &= 0b01111111; // velocityを0-127の範囲に収める
    ENTER_CRITICAL_SPINLOCK(messageQueueMutex);
    messageQueue.push_back(Message{MessageStatus::NOTE_OFF, channel, noteNo, velocity, 0});
    EXIT_CRITICAL_SPINLOCK(messageQueueMutex);
}
void Sampler::PitchBend(int16_t pitchBend, uint8_t channel)
{
    if (channel >= CH_COUNT) return; // 無効なチャンネルの場合は何もしない
    if (pitchBend < -8192) pitchBend = -8192;
    else if (pitchBend > 8191) pitchBend = 8191;
    ENTER_CRITICAL_SPINLOCK(messageQueueMutex);
    messageQueue.push_back(Message{MessageStatus::PITCH_BEND, channel, 0, 0, pitchBend});
    EXIT_CRITICAL_SPINLOCK(messageQueueMutex);
}

void Sampler::Channel::NoteOn(uint8_t noteNo, uint8_t velocity)
{
    LOGD("Sampler", "NoteOn : %2x, %2x", noteNo, velocity);
    // 空いているPlayerを探し、そのPlayerにサンプルをセットする
    uint_fast8_t oldestPlayerId = 0;
    
    // 弱参照からの共有ポインタ取得を試みる
    auto samplerPtr = sampler.lock();
    if (!samplerPtr) return;  // サンプラーが既に解放されている場合は何もしない
    
    ENTER_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
    for (uint_fast8_t i = 0; i < MAX_SOUND; i++)
    {
        if (samplerPtr->players[i].playing == false)
        {
            // チャンネル情報を追加
            uint8_t channelIndex = std::distance(&samplerPtr->channels[0], this);
            samplerPtr->players[i] = Sampler::SamplePlayer(timbre->GetAppropriateSample(noteNo, velocity), noteNo, velocityTable[velocity], pitchBend, channelIndex);
            playingNotes.push_back(PlayingNote{noteNo, i});
            EXIT_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
            return;
        }
        else
        {
            if (samplerPtr->players[i].createdAt < samplerPtr->players[oldestPlayerId].createdAt)
                oldestPlayerId = i;
        }
    }
    // 全てのPlayerが再生中だった時には、最も昔に発音されたPlayerを停止する
    uint8_t channelIndex = std::distance(&samplerPtr->channels[0], this);
    samplerPtr->players[oldestPlayerId] = Sampler::SamplePlayer(timbre->GetAppropriateSample(noteNo, velocity), noteNo, velocityTable[velocity], pitchBend, channelIndex);
    playingNotes.push_back(PlayingNote{noteNo, oldestPlayerId});
    EXIT_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
}
void Sampler::Channel::NoteOff(uint8_t noteNo, uint8_t velocity)
{
    LOGD("Sampler", "NoteOff: %2x, %2x", noteNo, velocity);
    
    // 弱参照からの共有ポインタ取得を試みる
    auto samplerPtr = sampler.lock();
    if (!samplerPtr) return;  // サンプラーが既に解放されている場合は何もしない
    
    ENTER_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
    // 現在このチャンネルで発音しているノートの中で該当するnoteNoのものの発音を終わらせる
    for (auto itr = playingNotes.begin(); itr != playingNotes.end();)
    {
        if (itr->noteNo == noteNo)
        {
            SamplePlayer *player = &(samplerPtr->players[itr->playerId]);
            // ノート番号とチャンネル両方が一致する場合、発音を終わらせる\
            // 発音後に同時発音数制限によって発音が止められている場合は何もしないことになる
            uint8_t channelIndex = std::distance(&samplerPtr->channels[0], this);
            if (player->noteNo == noteNo && player->channel == channelIndex)
                player->released = true;
            itr = playingNotes.erase(itr);
        }
        else itr++;
    }
    EXIT_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
}
void Sampler::Channel::PitchBend(int16_t b)
{
    pitchBend = b * 12.0f / 8192.0f;
    
    // 弱参照からの共有ポインタ取得を試みる
    auto samplerPtr = sampler.lock();
    if (!samplerPtr) return;  // サンプラーが既に解放されている場合は何もしない
    
    ENTER_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
    // 既に発音中のノートに対してピッチベンドを適用する
    for (auto itr = playingNotes.begin(); itr != playingNotes.end(); itr++)
    {
        SamplePlayer *player = &(samplerPtr->players[itr->playerId]);
        // 同じチャンネルのノートにのみ適用する
        uint8_t channelIndex = std::distance(&samplerPtr->channels[0], this);
        if (player->channel == channelIndex) {
            player->pitchBend = pitchBend;
            player->UpdatePitch();
        }
    }
    EXIT_CRITICAL_SEMAPHORE(samplerPtr->playersMutex);
}

void Sampler::SamplePlayer::UpdatePitch()
{
    if (!sample) return;

    float delta = noteNo - sample->root + pitchBend;
    pitch = ((powf(2.0f, delta / 12.0f)));
}
void Sampler::SamplePlayer::UpdateGain()
{
    if (!sample) return;

    const Sample& sampleRef = *sample;

    if (!sampleRef.adsrEnabled)
    {
        gain = volume;
        return;
    }

    float goal;
    if (released)
        adsrState = release;

    switch (adsrState)
    {
    case attack:
        gain += sampleRef.attack * volume;
        if (gain >= volume)
        {
            gain = volume;
            adsrState = decay;
        }
        break;
    case decay:
        goal = sampleRef.sustain * volume;
        gain = (gain - goal) * sampleRef.decay + goal;
        if ((gain - goal) < 0.001f)
        {
            adsrState = sustain;
            gain = goal;
        }
        break;
    case sustain:
        break;
    case release:
        gain *= sampleRef.release;
        if (gain < 0.001f)
        {
            gain = 0;
            playing = false;
        }
        break;
    }
}

// sampler_process_inner の動作時に必要なデータ類をまとめた構造体
// アセンブリ言語からアクセスするのでメンバの順序を変えないこと
struct sampler_process_inner_work_t
{
    const int16_t *src;
    float *dst;
    float pos_f;
    float gain;
    float pitch;
};

extern "C"
{
    // アセンブリ言語版を呼び出すための関数宣言
    // これをコメントアウトすると、アセンブリ言語版は呼ばれなくなり、C/C++版が呼ばれる
    void sampler_process_inner(sampler_process_inner_work_t *work, uint32_t length);
}

// なぜか関数が名前空間に入っている場合はweak属性が効かないので、ここでアーキテクチャを判定する
#if !defined ( __XTENSA__ )

// アセンブリ言語版と同様の処理を行うC/C++版の実装
// weak属性を付けることで、アセンブリ言語版があればそちらを使う
__attribute((weak, optimize("-O3")))
void sampler_process_inner(sampler_process_inner_work_t *work, uint32_t length)
{
    const int16_t *s = work->src;
    float *d = work->dst;
    float pos_f = work->pos_f;
    float gain = work->gain;
    float pitch = work->pitch;
    do
    {
        int32_t s0 = s[0];
        int32_t s1 = s[1];
        // 2点間の差分にpos_fを掛けて補間
        float val = s0 + (s1 - s0) * pos_f;
        // 音量係数gainを掛けたあと波形合成
        d[0] += val * gain;
        // 出力先をひとつ進める
        ++d;
        // pos_fをpitchぶん進める
        pos_f += pitch;
        // pos_fから整数部分を取り出す
        uint32_t intval = pos_f;
        // pos_fから整数部分を引くことで小数部分のみを取り出す
        pos_f -= intval;
        // サンプリング元データ取得位置を進める
        s += intval;
        // 処理したのでlengthを1減らす
    } while (--length);
    // 結果をworkに書き戻す
    work->src = s;
    work->dst = d;
    work->pos_f = pos_f;
}

#endif

__attribute((optimize("-O2")))
void Sampler::Process(int16_t* __restrict__ output)
{
    // キューを処理する
    ENTER_CRITICAL_SPINLOCK(messageQueueMutex);
    while (!messageQueue.empty())
    {
        Message message = messageQueue.front();
        messageQueue.pop_front();
        EXIT_CRITICAL_SPINLOCK(messageQueueMutex);
        
        // ミューテックスの外でメッセージを処理
        switch (message.status)
        {
        case MessageStatus::NOTE_ON:
            channels[message.channel].NoteOn(message.noteNo, message.velocity);
            break;
        case MessageStatus::NOTE_OFF:
            channels[message.channel].NoteOff(message.noteNo, message.velocity);
            break;
        case MessageStatus::PITCH_BEND:
            channels[message.channel].PitchBend(message.pitchBend);
            break;
        }
        
        ENTER_CRITICAL_SPINLOCK(messageQueueMutex);
    }
    EXIT_CRITICAL_SPINLOCK(messageQueueMutex);

    // 波形を生成
    float data[SAMPLE_BUFFER_SIZE] __attribute__ ((aligned (16))) = {0.0f};
    
    ENTER_CRITICAL_SEMAPHORE(playersMutex);
    for (uint_fast8_t i = 0; i < MAX_SOUND; i++)
    {
        SamplePlayer *player = &players[i];
        if (player->playing == false)
            continue;

        for (uint_fast8_t j = 0; j < SAMPLE_BUFFER_SIZE / ADSR_UPDATE_SAMPLE_COUNT; j++)
        {
            if (!player->sample) break;
            const Sample &sample = *player->sample;
            if (sample.adsrEnabled)
                player->UpdateGain();
            if (player->playing == false)
                break;

            float pitch = player->pitch;
            float gain = player->gain;

            // gainにマスターボリュームを適用しておく
            // 後処理で float から int16_t への変換時処理を行う際の高速化の都合で、事前に 65536倍しておく
            gain *= masterVolume * 65536;

            auto src = sample.sample.get();
            sampler_process_inner_work_t work = {&src[player->pos], &data[j * ADSR_UPDATE_SAMPLE_COUNT], player->pos_f, gain, pitch};
            // 波形生成処理を行う
            sampler_process_inner(&work, ADSR_UPDATE_SAMPLE_COUNT);

            int32_t loopEnd = sample.length;
            int32_t loopBack = 0;
            // adsrEnabledが有効の場合はループポイントを使用する。
            if (sample.adsrEnabled)
            {
                loopEnd = sample.loopEnd;
                loopBack = sample.loopStart - loopEnd;
            }

            // 現在のサンプル位置に基づいてposがどこまで進んだか求める
            uint32_t pos = work.src - src;

            // ループポイント or 終端を超えた場合の処理
            if (pos >= loopEnd)
            {
                if (loopBack == 0)
                { // ループポイントが設定されていない場合は終端として扱い再生を停止する
                    player->playing = false;
                    break;
                }
                do
                {
                    pos += loopBack;
                } while (pos >= loopEnd);
            }

            player->pos = pos;
            player->pos_f = work.pos_f;
        }
    }
    EXIT_CRITICAL_SEMAPHORE(playersMutex);

    { // マスターエフェクト処理
        reverb.Process(data, data);
    }

    { // 生成した波形をint16_tに変換して出力先に書き込む
#if CONFIG_IDF_TARGET_ESP32S3
        // ESP32S3の場合はSIMD命令を使って高速化
        __asm__ volatile (
        "   mov             a11, %1                 \n"  // dataポインタをa11にコピー
        "   beqz.n          %2, SAMPLER_LOOP_END    \n"
        "   loop            %2, SAMPLER_LOOP_END    \n"     // ループ開始
        "   ee.ldf.128.ip   f11,f10,f9, f8, a11, 16 \n"     // 元データ float 4個 読み、a11 アドレスを 16 加算
        "   ee.ldf.128.ip   f15,f14,f13,f12,a11, 16 \n"     // 元データ float 4個 読み、a11 アドレスを 16 加算
        "   trunc.s         a12,f8, 0               \n"     // float 4個を int32_t 4個に変換する
        "   trunc.s         a14,f10,0               \n"     // trunc.s は int32_t の範囲に収まるよう桁溢れ防止が行われる。
        "   trunc.s         a13,f9, 0               \n"     //
        "   trunc.s         a15,f11,0               \n"     //
        "   srli            a12,a12,16              \n"     // 偶数indexの値について 右16bitシフトし int16_t 化する
        "   srli            a14,a14,16              \n"     //
        "   s32i            a13,%0, 0               \n"     // 奇数indexの値を先に 32bit のまま偶数位置へ出力する。
        "   s32i            a15,%0, 4               \n"     // これにより右シフト処理を省略できる
        "   s16i            a12,%0, 0               \n"     // 先ほど奇数indexの値を出力した場所に 16bit 化した偶数indexの値を出力して上書きする。
        "   s16i            a14,%0, 4               \n"     //
        "   trunc.s         a12,f12,0               \n"     // float 4個を int32_t 4個に変換
        "   trunc.s         a14,f14,0               \n"     //
        "   trunc.s         a13,f13,0               \n"     //
        "   trunc.s         a15,f15,0               \n"     //
        "   srli            a12,a12,16              \n"     // 偶数indexの値について 右16bitシフトし int16_t 化する
        "   srli            a14,a14,16              \n"     //
        "   s32i            a13,%0, 8               \n"     // 奇数indexの値を先に 32bit のまま偶数位置へ出力する。
        "   s32i            a15,%0, 12              \n"     // これにより右シフト処理を省略できる
        "   s16i            a12,%0, 8               \n"     // 先ほど奇数indexの値を出力した場所に 16bit 化した偶数indexの値を出力して上書きする。
        "   s16i            a14,%0, 12              \n"     //
        "   addi            %0, %0, 16              \n"     //
        "SAMPLER_LOOP_END:                          \n"     //
        : // output-list            // アセンブリ言語からC/C++への受渡しは無し
            "+r" ( output )         //  %0 に変数 output の値を指定
        : // input-list             // C/C++からアセンブリ言語への受渡し
            "r" ( data ),           //  %1 に変数 data の値を指定
            "r" ( SAMPLE_BUFFER_SIZE>>3 )  // %2 にバッファ長 / 8 の値を設定
        : // clobber-list           //  値を書き換えたレジスタの申告
            "f8","f9","f10","f11","f12","f13","f14","f15",
            "a11","a12","a13","a14","a15","memory" //  書き変えたレジスタをコンパイラに知らせる
        );
#else
        auto o = output;
        auto d = data;
        for (int i = 0; i < SAMPLE_BUFFER_SIZE >> 2; i++)
        { // 1ループあたりの処理回数を増やすことで処理効率を上げる
          // float から int32_t への変換。この処理は内部でtrunc.sが使用され、int32_tの範囲に収まるように桁溢れが防止される。
            int32_t d1 = d[1];
            int32_t d0 = d[0];
            int32_t d3 = d[3];
            int32_t d2 = d[2];
            ((uint32_t *)o)[0] = d1;
            o[0] = d0 >> 16;
            ((uint32_t *)o)[1] = d3;
            o[2] = d2 >> 16;
            o += 4;
            d += 4;
        }
#endif
    }
}

}
}
