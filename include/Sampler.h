#pragma once

#include <list>
#include <deque>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <memory>
#include <optional>
#include <functional>
#if defined(FREERTOS)
#include <freertos/FreeRTOS.h>
#else
#include <mutex>
#endif

#include "EffectReverb.h"

// ADSR更新周期 (サンプル数)
// ※ ADSRの更新周期が短いほど、ADSRの変化が滑らかになりますが、CPU負荷が増加します
#define ADSR_UPDATE_SAMPLE_COUNT 64

// 1回の波形生成処理で出力するサンプル数 (ADSR_UPDATE_SAMPLE_COUNT の倍数であること)
// ※ この数値が大きいほど、波形生成処理の効率が向上しますが、操作してから発音されるまでのレイテンシが増加します
#define SAMPLE_BUFFER_SIZE (ADSR_UPDATE_SAMPLE_COUNT * 2)
#define SAMPLE_RATE 48000

#define MAX_SOUND 32 // 最大同時発音数
#define CH_COUNT 16  // サンプラーはMIDIと同様に16個のチャンネルを持つ

namespace capsule
{
namespace sampler
{
    unsigned long micros();

    enum SampleAdsr
    {
        attack,
        decay,
        sustain,
        release,
    };

    struct Sample
    {
        std::shared_ptr<const int16_t> sample;
        uint32_t length;
        uint8_t root;
        uint32_t loopStart;
        uint32_t loopEnd;

        bool adsrEnabled;
        float attack;
        float decay;
        float sustain;
        float release;

        // ===== ノート単位のSVFローパスフィルター =====
        // filterEnabled = false の場合、フィルター経路はスキップされ
        // 既存の波形生成パス(asm版含む)がそのまま使われる
        bool  filterEnabled    = false;
        // SoundFont 2 absolute cent (1200·log2(fc/8.176Hz))
        // 13500 cent ≈ 19.9kHz でほぼ素通しに相当
        float filterCutoffCent = 13500.0f;
        float filterResonance  = 0.707f;   // Q値 (k = 1/Q)
        // フィルターADSRエンベロープ(0..1)に乗算するcent変調量。正値前提
        float filterEnvAmount  = 0.0f;
        // フィルターADSR (振幅ADSRと同じスケール)
        float filterAttack  = 1.0f;
        float filterDecay   = 1.0f;
        float filterSustain = 1.0f;
        float filterRelease = 1.0f;

        // 通常はこちらのコンストラクタを使用してください
        Sample(std::shared_ptr<const int16_t> sample, uint32_t length, uint8_t root, uint32_t loopStart, uint32_t loopEnd, bool adsrEnabled, float attack, float decay, float sustain, float release)
            : sample{std::move(sample)}, length{length}, root{root}, loopStart{loopStart}, loopEnd{loopEnd}, adsrEnabled{adsrEnabled}, attack{attack}, decay{decay}, sustain{sustain}, release{release} {}
        // このコンストラクタを使用することで簡潔な初期化が可能です
        // データが解放されないことが保証されている場合にのみ使用してください
        Sample(const int16_t *sample, uint32_t length, uint8_t root, uint32_t loopStart, uint32_t loopEnd, bool adsrEnabled, float attack, float decay, float sustain, float release)
            : sample{sample}, length{length}, root{root}, loopStart{loopStart}, loopEnd{loopEnd}, adsrEnabled{adsrEnabled}, attack{attack}, decay{decay}, sustain{sustain}, release{release} {}
        // フィルター付きコンストラクタ (shared_ptr版)
        Sample(std::shared_ptr<const int16_t> sample, uint32_t length, uint8_t root, uint32_t loopStart, uint32_t loopEnd,
               bool adsrEnabled, float attack, float decay, float sustain, float release,
               bool filterEnabled,
               float filterCutoffCent, float filterResonance, float filterEnvAmount,
               float filterAttack, float filterDecay, float filterSustain, float filterRelease)
            : sample{std::move(sample)}, length{length}, root{root}, loopStart{loopStart}, loopEnd{loopEnd},
              adsrEnabled{adsrEnabled}, attack{attack}, decay{decay}, sustain{sustain}, release{release},
              filterEnabled{filterEnabled}, filterCutoffCent{filterCutoffCent}, filterResonance{filterResonance}, filterEnvAmount{filterEnvAmount},
              filterAttack{filterAttack}, filterDecay{filterDecay}, filterSustain{filterSustain}, filterRelease{filterRelease} {}
        // フィルター付きコンストラクタ (生ポインタ版)
        Sample(const int16_t *sample, uint32_t length, uint8_t root, uint32_t loopStart, uint32_t loopEnd,
               bool adsrEnabled, float attack, float decay, float sustain, float release,
               bool filterEnabled,
               float filterCutoffCent, float filterResonance, float filterEnvAmount,
               float filterAttack, float filterDecay, float filterSustain, float filterRelease)
            : sample{sample}, length{length}, root{root}, loopStart{loopStart}, loopEnd{loopEnd},
              adsrEnabled{adsrEnabled}, attack{attack}, decay{decay}, sustain{sustain}, release{release},
              filterEnabled{filterEnabled}, filterCutoffCent{filterCutoffCent}, filterResonance{filterResonance}, filterEnvAmount{filterEnvAmount},
              filterAttack{filterAttack}, filterDecay{filterDecay}, filterSustain{filterSustain}, filterRelease{filterRelease} {}
        Sample(Sample&& other)
            : sample{std::move(other.sample)}, length{other.length}, root{other.root}, loopStart{other.loopStart}, loopEnd{other.loopEnd},
              adsrEnabled{other.adsrEnabled}, attack{other.attack}, decay{other.decay}, sustain{other.sustain}, release{other.release},
              filterEnabled{other.filterEnabled}, filterCutoffCent{other.filterCutoffCent}, filterResonance{other.filterResonance}, filterEnvAmount{other.filterEnvAmount},
              filterAttack{other.filterAttack}, filterDecay{other.filterDecay}, filterSustain{other.filterSustain}, filterRelease{other.filterRelease} {}
    };

    // MIDI規格のプログラムに対応する概念
    // いわゆる音色(おんしょく)
    // サンプルの集合からなり、ノートナンバーとベロシティを指定するとサンプルがただ一つ定まる
    // 横軸がノートナンバー、縦軸がベロシティの平面に長方形をプロットしていくイメージ
    class Timbre
    {
    public:
        struct MappedSample
        {
            std::shared_ptr<const Sample> sample;
            uint8_t lowerNoteNo;   // このサンプルが選ばれるノートナンバーの下限(自身を含む)
            uint8_t upperNoteNo;   // このサンプルが選ばれるノートナンバーの上限(自身を含む)
            uint8_t lowerVelocity; // このサンプルが選ばれるベロシティの下限(自身を含む)
            uint8_t upperVelocity; // このサンプルが選ばれるベロシティの上限(自身を含む)

            MappedSample(std::shared_ptr<const Sample> sample, uint8_t lowerNoteNo, uint8_t upperNoteNo, uint8_t lowerVelocity, uint8_t upperVelocity)
                : sample{std::move(sample)}, lowerNoteNo{lowerNoteNo}, upperNoteNo{upperNoteNo}, lowerVelocity{lowerVelocity}, upperVelocity{upperVelocity} {}
            MappedSample(MappedSample&& other) : sample{std::move(other.sample)}, lowerNoteNo{other.lowerNoteNo}, upperNoteNo{other.upperNoteNo}, lowerVelocity{other.lowerVelocity}, upperVelocity{other.upperVelocity} {}
            MappedSample(const MappedSample& other) : sample{other.sample}, lowerNoteNo{other.lowerNoteNo}, upperNoteNo{other.upperNoteNo}, lowerVelocity{other.lowerVelocity}, upperVelocity{other.upperVelocity} {}
            MappedSample& operator=(MappedSample&& other) noexcept
            {
                if (this != &other)
                {
                    sample = std::move(other.sample);
                    lowerNoteNo = other.lowerNoteNo;
                    upperNoteNo = other.upperNoteNo;
                    lowerVelocity = other.lowerVelocity;
                    upperVelocity = other.upperVelocity;
                }
                return *this;
            }
        };
        // 通常はこちらのコンストラクタを使用してください
        Timbre(std::vector<std::unique_ptr<MappedSample>> *samples) : samples{std::move(std::unique_ptr<std::vector<std::unique_ptr<MappedSample>>>(samples))} {}
        // このコンストラクタを使用することで簡潔な初期化が可能です
        // samplesや内部のサンプルが解放されないことが保証されている場合にのみ使用してください
        Timbre(std::vector<MappedSample> mss) : samples{std::make_unique<std::vector<std::unique_ptr<MappedSample>>>()}
        {
            for (auto &ms : mss)
            {
                samples->push_back(std::make_unique<MappedSample>(std::move(ms)));
            }
        }
        // 指定したノートナンバーとベロシティが範囲に含まれているサンプルへのshared_ptrを返す
        // 該当するサンプルがない場合はnullptrを返す
        std::shared_ptr<const Sample> GetAppropriateSample(uint8_t noteNo, uint8_t velocity);

    private:
        // サンプルの集合
        // 下記の制約をすべて満たしているものとします
        // * 任意の2つを取り出したとき、それらのノートナンバーの範囲が完全に一致しているか、全く重複していないかのどちらかである
        // * 同じlowerNoteNoを持つ任意の2つを取り出したとき、それらのベロシティの範囲が重複していない
        // * lowerNoteNoの低い順に並んでおり、同じlowerNoteNoを持つ項目はlowerVelocityの低い順に並んでいる
        std::unique_ptr<std::vector<std::unique_ptr<MappedSample>>> samples;
    };

    class Sampler : public std::enable_shared_from_this<Sampler>
    {
    public:
        // 与えられたサンプルを再生する
        class SamplePlayer
        {
        public:
            SamplePlayer(std::shared_ptr<const Sample> sample, uint8_t noteNo, float volume, float pitchBend, uint8_t channel)
                : sample{std::move(sample)}, noteNo{noteNo}, volume{volume}, pitchBend{pitchBend}, channel{channel}, createdAt{sampler::micros()}
            {
                UpdatePitch();
                // 振幅ADSRとフィルタADSRは同じ設計でプリシードする:
                //  - ADSR無効: 即座にピーク値で固定 (UpdateGainは呼ばれないため初期値で確定する)
                //  - attack >= 1.0 (即時アタック): ピーク値+decay状態でシード。NoteOn直後にNoteOffされても
                //    ピーク値から自然に減衰する。
                //  - それ以外 (スローアタック): 0+attack状態で開始し、UpdateGain/UpdateFilterEnv内で
                //    本来のランプ処理に任せる。NoteOn直後にNoteOffされた場合は0からreleaseするので
                //    実質的にほぼ無音になるが、これは「アタック完了前に離鍵された」という挙動として妥当。
                if (!this->sample || !this->sample->adsrEnabled)
                {
                    gain = volume;
                }
                else if (this->sample->attack >= 1.0f)
                {
                    gain = volume;
                    adsrState = decay;
                }
                else
                {
                    gain = 0.0f;
                    // adsrState はメンバ初期値の attack のまま
                }
                if (this->sample && this->sample->filterEnabled && this->sample->filterAttack >= 1.0f)
                {
                    filterEnv = 1.0f;
                    filterAdsrState = decay;
                }
            }
            SamplePlayer() : sample{nullptr}, noteNo{60}, volume{1.0f}, channel{0}, createdAt{sampler::micros()}, playing{false} {}
            std::shared_ptr<const Sample> sample;
            uint8_t noteNo;
            float volume;
            float pitchBend = 0;
            uint8_t channel = 0;   // 音を鳴らしたチャンネル
            unsigned long createdAt = 0;
            bool released = false;

            bool playing = true;
            uint32_t pos = 0;
            float pos_f = 0.0f;
            float gain = 0.0f; // volumeとADSR処理により算出される値
            float pitch = 1.0f; // noteNoとpitchBendにより算出される値
            enum SampleAdsr adsrState = SampleAdsr::attack;

            // SVF 内部状態 (積分器メモリ)
            float ic1eq = 0.0f;
            float ic2eq = 0.0f;
            // フィルターADSRエンベロープ (0..1)
            float filterEnv = 0.0f;
            enum SampleAdsr filterAdsrState = SampleAdsr::attack;

            void UpdateGain();
            void UpdatePitch();
            void UpdateFilterEnv();
        private:
        };

        // MIDI規格のチャンネルに対応する概念
        class Channel
        {
        public:
            Channel() {}
            Channel(std::weak_ptr<Sampler> sampler) : sampler{std::move(sampler)} {}
            void NoteOn(uint8_t noteNo, uint8_t velocity);
            void NoteOff(uint8_t noteNo, uint8_t velocity);
            void PitchBend(int16_t pitchBend);
            void SetTimbre(std::shared_ptr<Timbre> t);

        private:
            std::weak_ptr<Sampler> sampler; // 循環参照を避けるために弱参照を使用
            struct PlayingNote
            {
                uint8_t noteNo;
                uint_fast8_t playerId;
            };
            std::shared_ptr<Timbre> timbre;
            float pitchBend = 0.0f;
            std::list<PlayingNote> playingNotes; // このチャンネルで現在再生しているノート
        };
        
        // shared_ptrを生成するファクトリー関数
        // コンストラクタではなくこちらを使用しないと正しく動作しません
        static std::shared_ptr<Sampler> Create()
        {
            auto sampler = std::make_shared<Sampler>();
            sampler->initialize();
            return sampler;
        }
        
    private:        
        void initialize()
        {
            for (uint_fast8_t i = 0; i < CH_COUNT; i++)
                channels[i] = Channel(weak_from_this());
#if defined(FREERTOS)
            InitializeMutexes();
#endif
        }
        
    public:

        void NoteOn(uint8_t noteNo, uint8_t velocity, uint8_t channel);
        void NoteOff(uint8_t noteNo, uint8_t velocity, uint8_t channel);
        void PitchBend(int16_t pitchBend, uint8_t channel);
        void SetTimbre(uint8_t channel, std::shared_ptr<Timbre> t);

        void Process(int16_t *output);

        float masterVolume = 0.4f;

    private:
        // 各メッセージのキューイングに使用する
        // MIDIのメッセージとは互換性がない
        // TODO: noteNo/velocityとpitchBendは同時に使われることがないのに両方メモリを占有しているのどうにかならないか…？
        struct Message
        {
            uint8_t status;
            uint8_t channel;
            uint8_t noteNo;
            uint8_t velocity;
            int16_t pitchBend;
        };
        enum MessageStatus
        {
            NOTE_ON,
            NOTE_OFF,
            PITCH_BEND
        };
        Channel channels[CH_COUNT]; // コンストラクタで初期化する
        SamplePlayer players[MAX_SOUND] = {SamplePlayer()};
        // 受け取ったNoteOn/NoteOff/PitchBendなどは一旦キューに入れておき、Processのタイミングで処理する
        // これにより、Processを別スレッドで動かすことができる
        // TODO: messageQueue自体の排他制御は必要ない？
        std::deque<Message> messageQueue;
#if defined(FREERTOS)
        portMUX_TYPE messageQueueMutex = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t playersMutex = NULL;
#else
        std::mutex messageQueueMutex;
        std::mutex playersMutex;
#endif

        EffectReverb reverb = EffectReverb(0.4f, 0.5f, SAMPLE_BUFFER_SIZE, SAMPLE_RATE);
        
        // コンストラクタでFreeRTOSセマフォを初期化するためのメソッド
        void InitializeMutexes();
    };
    
}
}
