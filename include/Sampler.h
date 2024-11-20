#pragma once

#include <list>
#include <deque>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <memory>
#include <freertos/FreeRTOS.h>

#include <EffectReverb.h>
#include <Utils.h>

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
    enum SampleAdsr
    {
        attack,
        decay,
        sustain,
        release,
    };

    struct Sample
    {
        std::unique_ptr<const int16_t> sample;
        uint32_t length;
        uint8_t root;
        uint32_t loopStart;
        uint32_t loopEnd;

        bool adsrEnabled;
        float attack;
        float decay;
        float sustain;
        float release;
    
        Sample(const int16_t *sample, uint32_t length, uint8_t root, uint32_t loopStart, uint32_t loopEnd, bool adsrEnabled, float attack, float decay, float sustain, float release)
            : sample{sample}, length{length}, root{root}, loopStart{loopStart}, loopEnd{loopEnd}, adsrEnabled{adsrEnabled}, attack{attack}, decay{decay}, sustain{sustain}, release{release} {}
        Sample(Sample&& other) : sample{std::move(other.sample)}, length{other.length}, root{other.root}, loopStart{other.loopStart}, loopEnd{other.loopEnd}, adsrEnabled{other.adsrEnabled}, attack{other.attack}, decay{other.decay}, sustain{other.sustain}, release{other.release} {}
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

            MappedSample(const Sample *sample, uint8_t lowerNoteNo, uint8_t upperNoteNo, uint8_t lowerVelocity, uint8_t upperVelocity)
                : sample{sample}, lowerNoteNo{lowerNoteNo}, upperNoteNo{upperNoteNo}, lowerVelocity{lowerVelocity}, upperVelocity{upperVelocity} {}
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
        // 指定したノートナンバーとベロシティが範囲に含まれているサンプルを返す
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

    class Sampler
    {
    public:
        // 与えられたサンプルを再生する
        class SamplePlayer
        {
        public:
            SamplePlayer(std::shared_ptr<const Sample> sample, uint8_t noteNo, float volume, float pitchBend)
                : sample{sample}, noteNo{noteNo}, volume{volume}, pitchBend{pitchBend}, createdAt{sampler::micros()}
            {
                UpdatePitch();
                gain = volume;
            }
            SamplePlayer() : sample{nullptr}, noteNo{60}, volume{1.0f}, createdAt{sampler::micros()}, playing{false} {}
            std::shared_ptr<const Sample> sample;
            uint8_t noteNo;
            float volume;
            float pitchBend = 0;
            unsigned long createdAt = 0;
            bool released = false;

            bool playing = true;
            uint32_t pos = 0;
            float pos_f = 0.0f;
            float gain = 0.0f; // volumeとADSR処理により算出される値
            float pitch = 1.0f; // noteNoとpitchBendにより算出される値
            enum SampleAdsr adsrState = SampleAdsr::attack;

            void UpdateGain();
            void UpdatePitch();
        private:
        };

        // MIDI規格のチャンネルに対応する概念
        class Channel
        {
        public:
            Channel() {}
            Channel(Sampler *sampler) : sampler{sampler} {}
            void NoteOn(uint8_t noteNo, uint8_t velocity);
            void NoteOff(uint8_t noteNo, uint8_t velocity);
            void PitchBend(int16_t pitchBend);
            void SetTimbre(std::shared_ptr<Timbre> t);

        private:
            Sampler *sampler; // 親サンプラー
            struct PlayingNote
            {
                uint8_t noteNo;
                uint_fast8_t playerId;
            };
            std::shared_ptr<Timbre> timbre;
            float pitchBend = 0.0f;
            std::list<PlayingNote> playingNotes; // このチャンネルで現在再生しているノート
        };

        Sampler()
        {
            for (uint_fast8_t i = 0; i < CH_COUNT; i++)
                channels[i] = Channel(this);
        }
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
        portMUX_TYPE messageQueueMutex = portMUX_INITIALIZER_UNLOCKED;

        EffectReverb reverb = EffectReverb(0.4f, 0.5f, SAMPLE_BUFFER_SIZE, SAMPLE_RATE);
    };
    
}
}
