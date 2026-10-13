// Эмулятор Norpak TTX650: машина вокруг процессора 6809 (см. emu_norpak.h).
#include "emu_norpak.h"
#include "util.h"
#include <cstring>
#include <stdexcept>

// такты шины E за поле: 1.431818 МГц / 59.94 Гц = 91 такт × 262.5 строки
static const long FIELD_CYCLES = 91 * 262 + 45;
static const int LINE_CYCLES = 91;

Norpak::Norpak(const std::vector<uint8_t> &rom) : rom_(rom), ram_(0x8000, 0), vram_(0x8000, 0) {
    rom_.resize(0x10000, 0xFF);
    eeprom.fill(0xFFFF);
    cpu.bus = this;
    reset();
}

void Norpak::reset() {
    std::fill(ram_.begin(), ram_.end(), 0);
    bank_ = 0; mode_ = 0x0F; x_ = y_ = 0; lut_addr_ = 0; switch_ = 0; yscroll_ = 0; ttx_ptr_ = ttx_cnt_ = 0; page_ = 7;
    lut_r_.fill(0); lut_g_.fill(0); lut_b_.fill(0); lut_x_.fill(0);
    irq_stat_ = 0; memset(ctl_, 0, sizeof ctl_); e0_ = e2_ = 0; key_ = 0;
    acia_cmd_ = acia_ctl_ = acia_rx_ = 0; acia_rdrf_ = false; srx_.clear();
    rx_.clear(); keys_.clear(); fields = 0; field_cycle_ = 0;
    cpu.irq = cpu.firq = false;
    cpu.reset();
}

uint8_t Norpak::peek(uint16_t a) {
    if (a >= 0xC000) return rom_[a];
    if (a >= 0x8000) return rom_[(bank_ & 3) * 0x4000 + (a - 0x8000)];
    if (a < 0x100) return 0xFF;
    if (a < 0x2000 && !(page_ & 4)) return vram_[(page_ & 3) * 0x2000 + a];
    return ram_[a];
}

uint8_t Norpak::rd(uint16_t a) {
    if (a >= 0xC000) return rom_[a];
    if (a >= 0x8000) return rom_[(bank_ & 3) * 0x4000 + (a - 0x8000)];
    if (a < 0x100) return io_rd((uint8_t)a);
    if (a < 0x2000 && !(page_ & 4)) return vram_[(page_ & 3) * 0x2000 + a];
    return ram_[a];
}

void Norpak::wr(uint16_t a, uint8_t v) {
    if (a >= 0x8000) return;
    if (a < 0x100) { io_wr((uint8_t)a, v); return; }
    if (a < 0x2000 && !(page_ & 4)) { vram_[(page_ & 3) * 0x2000 + a] = v; return; }
    ram_[a] = v;
}

// ---------------------------------------------------------------- R6549
// Регистры по адресам платы: $0001 — режим/состояние, $0004 — X, $0003 — Y (в режиме 5 — указатель телетекста),
// $0002 — данные (точки в режиме 0, параметр в режимах 1–6).

uint8_t Norpak::cvdg_rd(int reg) {
    int m = mode_ & 7;
    switch (reg) {
    case 0: {
        long fc = field_cycle_ / cpu_mult, c = fc % LINE_CYCLES, line = fc / LINE_CYCLES;
        bool vis_v = line >= 21 && line < 21 + 210, vis_h = c < 64;
        return (uint8_t)((vis_v ? 0x80 : 0) | (vis_h ? 0x40 : 0) | (m << 3) | (page_ & 7));
    }
    case 1: return x_;
    case 2: return m == 5 ? ttx_ptr_ : y_;
    case 3:
        if (m == 0) {
            if (y_ > 255) return 0;
            return vbyte(x_, y_);
        }
        if (m == 2) {
            int i = lut_addr_ & 15;
            if (lut_addr_ & 0x40) return lut_r_[i];
            if (lut_addr_ & 0x20) return lut_g_[i];
            if (lut_addr_ & 0x10) return lut_b_[i];
            if (lut_addr_ & 0x80) return lut_x_[i] ? 8 : 0;
        }
        return 0xFF;
    }
    return 0xFF;
}

void Norpak::cvdg_wr(int reg, uint8_t v) {
    int m = mode_ & 7;
    switch (reg) {
    case 0: mode_ = v & 0x0F; break;
    case 1: if (m == 0) x_ = v; break;
    case 2: if (m == 5) { ttx_ptr_ = v; ttx_cnt_ = 0; } else if (m == 0) y_ = v; break;
    case 3:
        switch (m) {
        case 0: {
            uint8_t &b = vbyte(x_, y_);
            if (mode_ & 8) { b = v; x_ += 2; }                       // байтовый подрежим: две точки, X += 2
            else if (x_ & 1) b = (uint8_t)((b & 0x0F) | (v & 0xF0)); // нечётная точка — старшая тетрада
            else b = (uint8_t)((b & 0xF0) | (v & 0x0F));
            break;
        }
        case 1: lut_addr_ = v; break;
        case 2: {
            int i = lut_addr_ & 15;
            if (lut_addr_ & 0x80) lut_x_[i] = (v >> 3) & 1;
            if (lut_addr_ & 0x40) lut_r_[i] = v & 15;
            if (lut_addr_ & 0x20) lut_g_[i] = v & 15;
            if (lut_addr_ & 0x10) lut_b_[i] = v & 15;
            break;
        }
        case 3: switch_ = v; break;
        case 4: yscroll_ = v; break;
        case 6: page_ = v & 7; break;
        }
        break;
    }
}

static int cvdg_reg(uint8_t a) { return a == 1 ? 0 : a == 4 ? 1 : a == 3 ? 2 : 3; }

// ---------------------------------------------------------------- порты платы

uint8_t Norpak::io_rd(uint8_t a) {
    if (a >= 1 && a <= 4) return cvdg_rd(cvdg_reg(a));
    switch (a) {
    case 0x80: { uint8_t s = irq_stat_; irq_stat_ = 0; cpu.irq = false; return s; }
    case 0x85: return bank_;
    case 0x86: return (uint8_t)key_;
    case 0xC0: acia_rdrf_ = false; return acia_rx_;
    case 0xC1: return (uint8_t)((acia_irq() ? 0x80 : 0) | 0x10 | (acia_rdrf_ ? 0x08 : 0));   // передатчик всегда свободен
    case 0xC2: return acia_cmd_;
    case 0xC3: return acia_ctl_;
    case 0xE1: return (uint8_t)(ee_do_ ? 0x08 : 0);
    }
    if (a >= 0x80 && a < 0x88) return ctl_[a & 7];
    return 0xFF;
}

void Norpak::io_wr(uint8_t a, uint8_t v) {
    if (a >= 1 && a <= 4) { cvdg_wr(cvdg_reg(a), v); return; }
    switch (a) {
    case 0x85: bank_ = v; return;
    case 0xC0: if (v == '\r' || v == '\n' || (v >= 32 && v < 127)) log += (char)v; return;
    case 0xE0: e0_ = v; return;
    case 0xE2: e2_ = v; ee_clock(v); return;
    }
    if (a >= 0x80 && a < 0x88) ctl_[a & 7] = v;
}

// ACIA: прерывание при принятом байте (бит 1 команды = 0) или свободном передатчике (биты 3–2 = 01), DTR (бит 0) включён
bool Norpak::acia_irq() const {
    if (!(acia_cmd_ & 1)) return false;
    if (acia_rdrf_ && !(acia_cmd_ & 2)) return true;
    return (acia_cmd_ & 0x0C) == 0x04;
}

// ---------------------------------------------------------------- EEPROM 93C46 (64 × 16 бит)

void Norpak::set_config(const std::vector<uint8_t> &b) {
    uint16_t sum = 0;
    for (int i = 0; i < 15; i++) { eeprom[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]); sum += eeprom[i]; }
    eeprom[15] = (uint16_t)(0xAAAA - sum);
}

void Norpak::ee_clock(uint8_t v) {
    bool cs = v & 1, sk = v & 2, rise = sk && !(ee_last_ & 2);
    ee_last_ = v;
    if (!cs) { ee_state_ = 0; ee_bits_ = 0; ee_sr_ = 0; ee_do_ = 1; return; }
    if (!rise) return;
    int di = (v >> 4) & 1;
    switch (ee_state_) {
    case 0: if (di) { ee_state_ = 1; ee_bits_ = 0; ee_sr_ = 0; } break;   // стартовый бит
    case 1:
        ee_sr_ = ee_sr_ << 1 | di;
        if (++ee_bits_ == 8) {
            int op = ee_sr_ >> 6, a = ee_sr_ & 63;
            ee_state_ = 0;
            if (op == 2) { ee_word_ = eeprom[a]; ee_out_ = 0; ee_do_ = 0; ee_state_ = 3; }    // READ: сначала фиктивный 0
            else if (op == 1) { ee_word_ = (uint16_t)a; ee_bits_ = 0; ee_sr_ = 0; ee_state_ = 2; }   // WRITE
            else if (op == 3) { if (ee_we_) eeprom[a] = 0xFFFF; }                            // ERASE
            else switch (a >> 4) {
                case 0: ee_we_ = false; break;
                case 3: ee_we_ = true; break;
                case 2: if (ee_we_) eeprom.fill(0xFFFF); break;
                case 1: ee_word_ = 0xFFFF; ee_bits_ = 0; ee_sr_ = 0; ee_state_ = 2; break;    // WRAL
            }
        }
        break;
    case 2:   // 16 бит данных записи
        ee_sr_ = ee_sr_ << 1 | di;
        if (++ee_bits_ == 16) {
            if (ee_we_) { if (ee_word_ == 0xFFFF) eeprom.fill((uint16_t)ee_sr_); else eeprom[ee_word_ & 63] = (uint16_t)ee_sr_; }
            ee_state_ = 0;
        }
        break;
    case 3:   // выдача 16 бит, старший первым
        ee_do_ = ee_out_ < 16 ? (ee_word_ >> (15 - ee_out_)) & 1 : 0;
        ee_out_++;
        break;
    }
}

// ---------------------------------------------------------------- приём и кадры

void Norpak::feed(const uint8_t *p) {
    // в буфер DMA ложатся 32 байта: второй и третий байты адреса, CI, PS и 28 байтов данных (первый байт адреса
    // плата не сохраняет — по коду ПЗУ: адрес в буфере сравнивается двумя байтами, данные начинаются с 4-го)
    std::array<uint8_t, 32> a; memcpy(a.data(), p + 1, 32);
    rx_.push_back(a);
}

bool Norpak::dma_packet() {
    if (rx_.empty() || ctl_[4] != 1) return false;     // $0084 = 1 — приём разрешён
    auto &p = rx_.front();
    uint16_t base = (uint16_t)(ttx_ptr_ << 5);
    for (int i = 0; i < 32; i++) vram_[3 * 0x2000 + ((base + i) & 0x1FFF)] = p[i];   // DMA пишет в видеостраницу V11 (её невидимую часть)
    ttx_ptr_++;
    rx_.pop_front();
    packets_taken++;
    irq_stat_ |= 0x01;
    return true;
}

void Norpak::run_field() {
    // кадровое гашение: таймер и приём пакетов
    irq_stat_ |= 0x02;
    for (int i = 0; i < packets_per_field; i++) if (!dma_packet()) break;
    if (!keys_.empty() && !(irq_stat_ & 0x04) && fields % 6 == 0) { key_ = keys_.front(); keys_.pop_front(); irq_stat_ |= 0x04; }
    cpu.irq = irq_stat_ != 0;
    for (field_cycle_ = 0; field_cycle_ < FIELD_CYCLES * cpu_mult;) {
        if (cover) (*cover)[(cpu.PC >= 0x8000 && cpu.PC < 0xC000 ? (bank_ & 3) : 3) << 16 | cpu.PC]++;
        field_cycle_ += cpu.step();
        if (cpu.illegal) break;
        cpu.irq = irq_stat_ != 0;
        if (!acia_rdrf_ && !srx_.empty() && field_cycle_ / cpu_mult % LINE_CYCLES < 8) { acia_rx_ = srx_.front(); srx_.pop_front(); acia_rdrf_ = true; }
        cpu.firq = acia_irq();
    }
    fields++;
}

Image Norpak::screen() const {
    Image im(256, 210);
    for (int r = 0; r < 210; r++) {
        int y = yscroll_ - r; if (y < 0) y += 210;
        for (int x = 0; x < 256; x++) {
            uint8_t b = vbyte(x, y);
            int c = (x & 1) ? b >> 4 : b & 15;
            im.set(x, r, (uint32_t)(lut_r_[c] * 17) << 16 | (uint32_t)(lut_g_[c] * 17) << 8 | (uint32_t)(lut_b_[c] * 17));
        }
    }
    return im;
}

// ---------------------------------------------------------------- декодер с потоком

static std::vector<uint8_t> norpak_rom() {
    Bytes b = resource_bytes("NORPAK_ROM");
    if (b.size() != 0x10000) throw std::runtime_error("the Norpak TTX650 ROM is missing from the program");
    return std::vector<uint8_t>(b.begin(), b.end());
}

NorpakBox::NorpakBox(const std::string &t33) : m(norpak_rom()) {
    Bytes b = read_file(t33);
    if (b.size() < 33 || b.size() % 33) throw std::runtime_error("not a NABTS .t33 stream (size is not a multiple of 33 bytes)");
    t33_.assign(b.begin(), b.end());
}

void NorpakBox::run(int fields) {
    for (int i = 0; i < fields; i++) {
        while ((int)m.queued() < 2 * m.packets_per_field) {
            if (pos_ + 33 > t33_.size()) { if (!loop) break; pos_ = 0; }
            m.feed(&t33_[pos_]); pos_ += 33; sent_++;
        }
        m.run_field();
        if (m.cpu.illegal) throw std::runtime_error(fmt("the decoder program stopped: unknown 6809 instruction at %04X", m.cpu.PC));
    }
}

void NorpakBox::press(const std::string &k) { for (char c : k) m.key(c == '\n' ? '\r' : (uint8_t)c); }

const std::vector<NorpakBox::Key> &NorpakBox::keys() {
    static const std::vector<Key> K = {
        {"Index", "^", "index page of the service"}, {"Back", "<", "previous page"}, {"Next", "&", "next page"},
        {"Hold", "\"", "pause the page cycle (PAUSE)"}, {"Auto", "$", "pages change by themselves (AUTO)"},
        {"Caption", ">", "captions (CAPTION)"}, {"Flash", "%", "news flash (FLASH)"},
        {"1", "1", ""}, {"2", "2", ""}, {"3", "3", ""}, {"4", "4", ""}, {"5", "5", ""}, {"6", "6", ""}, {"7", "7", ""}, {"8", "8", ""}, {"9", "9", ""},
        {"*", "*", ""}, {"0", "0", ""}, {"#", "#", ""}, {"Enter", "\r", "go to the page number typed"},
        {"Test", "Z", "test pattern with the ROM version"}, {"Reset", "=", "restart the decoder"}};
    return K;
}
