// Эмулятор декодера NABTS Norpak TTX650 (ПЗУ 36-08948-01 REV D): процессор 6809, видеочип Rockwell R6549 CVDG,
// приём пакетов NABTS через DMA видеочипа, прерывания кадра, пульт.
// Карта памяти (по коду ПЗУ): $0000–$00FF — порты, $0100–$7FFF — ОЗУ (при P = 0 окно $0000–$1FFF — видеостраница),
// $8000–$BFFF — банк ПЗУ (номер в $0085), $C000–$FFFF — постоянная часть ПЗУ.
#pragma once
#include "m6809.h"
#include "png.h"
#include <array>
#include <deque>
#include <string>
#include <vector>

class Norpak : public Bus6809 {
public:
    explicit Norpak(const std::vector<uint8_t> &rom);
    void reset();
    void run_field();                          // одно поле (1/59.94 с): процессор, кадровое прерывание, приём пакетов
    void feed(const uint8_t *pkt33);           // пакет NABTS (33 байта после кода синхронизации, как в .t33) в очередь приёма
    size_t queued() const { return rx_.size(); }
    void key(int code) { keys_.push_back(code); }   // код клавиши пульта (порт $0086)
    // настройки в последовательной EEPROM 93C46: 15 слов + контрольное (0xAAAA − сумма); байт i — как в ОЗУ после чтения
    void set_config(const std::vector<uint8_t> &bytes30);
    std::array<uint16_t, 64> eeprom;   // стёртая (0xFFFF): контрольная сумма не сходится, ПЗУ берёт заводские настройки
    void serial_in(int c) { srx_.push_back((uint8_t)c); }   // байт в последовательный порт (терминал)
    Image screen() const;                      // 256 × 210, цвета из таблицы LUT

    uint8_t rd(uint16_t a) override;
    void wr(uint16_t a, uint8_t v) override;
    uint8_t peek(uint16_t a) override;

    M6809 cpu;
    long fields = 0, packets_taken = 0;
    std::string log;                           // вывод последовательного порта
    std::vector<uint32_t> *cover = nullptr;    // отладка: счётчик исполнения по (банк << 16 | PC)
    int cpu_mult = 1;                          // «разгон»: во столько раз больше тактов процессора за поле (рисует быстрее, поток — как в эфире)
    int packets_per_field = 4;                 // строк NABTS в гашении поля (сколько пакетов успевает принять DMA)
private:
    std::vector<uint8_t> rom_;
    std::vector<uint8_t> ram_, vram_;          // 32 КБ программы, 32 КБ видео (4 страницы по 8 КБ)
    uint8_t bank_ = 0;
    // R6549
    uint8_t mode_ = 0x0F, x_ = 0, y_ = 0, lut_addr_ = 0, switch_ = 0, yscroll_ = 0, ttx_ptr_ = 0, ttx_cnt_ = 0, page_ = 7;
    std::array<uint8_t, 16> lut_r_{}, lut_g_{}, lut_b_{}, lut_x_{};
    // порты платы
    uint8_t irq_stat_ = 0, ctl_[8] = {0}, e0_ = 0, e2_ = 0;
    // ACIA 6551 ($00C0–$00C3), линия FIRQ
    uint8_t acia_cmd_ = 0, acia_ctl_ = 0, acia_rx_ = 0;
    bool acia_rdrf_ = false;
    std::deque<uint8_t> srx_;
    bool acia_irq() const;
    // EEPROM: CS — бит 0, SK — бит 1, DI — бит 4 порта $00E2, DO — бит 3 порта $00E1
    uint8_t ee_last_ = 0; int ee_bits_ = 0; uint32_t ee_sr_ = 0; int ee_out_ = 0; uint16_t ee_word_ = 0; int ee_state_ = 0; bool ee_we_ = false; int ee_do_ = 1;
    void ee_clock(uint8_t v);
    std::deque<std::array<uint8_t, 32>> rx_;
    std::deque<int> keys_;
    int key_ = 0;
    long field_cycle_ = 0;
    uint8_t vbyte(int x, int y) const { return vram_[(size_t)(y >> 6) * 0x2000 + (y & 63) * 128 + (x >> 1)]; }
    uint8_t &vbyte(int x, int y) { return vram_[(size_t)(y >> 6) * 0x2000 + (y & 63) * 128 + (x >> 1)]; }
    uint8_t cvdg_rd(int reg);
    void cvdg_wr(int reg, uint8_t v);
    uint8_t io_rd(uint8_t a);
    void io_wr(uint8_t a, uint8_t v);
    bool dma_packet();
};

// Декодер целиком: ПЗУ из ресурсов программы, поток NABTS .t33 в роли антенны (пакеты по строкам гашения,
// по кругу, как карусель в эфире), пульт.
struct NorpakBox {
    explicit NorpakBox(const std::string &t33);
    void run(int fields);                      // n полей (60 в секунду)
    void press(const std::string &keys);       // клавиши пульта по очереди
    Image screen() const { return m.screen(); }
    double seconds() const { return m.fields / 59.94; }
    size_t packets_total() const { return t33_.size() / 33; }
    size_t packets_sent() const { return sent_; }
    bool loop = true;                          // поток кончился — начать сначала
    Norpak m;
    // клавиши пульта TTX650 (коды ASCII, как их передаёт процессор пульта)
    struct Key { const char *label; const char *code; const char *hint; };
    static const std::vector<Key> &keys();
private:
    std::vector<uint8_t> t33_;
    size_t pos_ = 0, sent_ = 0;
};
