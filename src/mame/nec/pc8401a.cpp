// license:BSD-3-Clause
// copyright-holders:Curt Coder
/*

    NEC PC-8401A-LS "Starlet"
    NEC PC-8500 "Studley"

    TODO:

    - keyboard interrupt
    - RTC TP pulse
    - clock does not advance in menu (timer irq?)
    - modernize memory map
    - mirror e800-ffff to 6800-7fff
    - soft power on/off
    - NVRAM
    - 8251 USART
    - 8255 ports
    - Merge keyboard with pc8001 / pc8801 / pc88va (same keys, running on a MCU like VA)
    - PC-8431A FDC is same family as PC-80S31K, basically the 3.5" version of it.
      Likely none of the available BIOSes fits here.

    - peripherals
        * PC-8431A Dual Floppy Drive
        * PC-8441A CRT / Disk Interface (MC6845, monochrome)
        * PC-8461A 1200 Baud Modem
        * PC-8407A 128KB RAM Expansion
        * PC-8508A ROM/RAM Cartridge

    - Use the 600 baud save rate (PIP CAS2:=A:<filename.ext> this is more reliable than the 1200 baud (PIP CAS:=A:<filename.ext> rate.

*/

#include "emu.h"
#include "bus/rs232/rs232.h"

#include "cpu/z80/z80.h"
#include "machine/i8255.h"
#include "machine/i8251.h"
#include "machine/ram.h"
#include "machine/timer.h"
#include "machine/upd1990a.h"
#include "video/mc6845.h"
#include "video/sed1330.h"

#include "bus/generic/slot.h"
#include "bus/generic/carts.h"

#include "emupal.h"
#include "screen.h"
#include "utf8.h"

#define SCREEN_TAG      "screen"
#define CRT_SCREEN_TAG  "screen2"

#define Z80_TAG         "z80"
#define UPD1990A_TAG    "upd1990a"
#define AY8910_TAG      "ay8910"
#define SED1330_TAG     "sed1330"
#define MC6845_TAG      "mc6845"
#define I8251_TAG       "i8251"
#define RS232_TAG       "rs232"

#define PC8401A_CRT_VIDEORAM_SIZE   0x2000

namespace {

class pc8401a_state : public driver_device
{
public:
	pc8401a_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_maincpu(*this, Z80_TAG)
		, m_rtc(*this, UPD1990A_TAG)
		, m_lcdc(*this, SED1330_TAG)
		, m_screen(*this, SCREEN_TAG)
		, m_cart(*this, "cartslot")
		, m_io_cart(*this, "io_cart")
		, m_ram(*this, RAM_TAG)
		, m_rom(*this, Z80_TAG)
		, m_crt_ram(*this, "crt_ram", PC8401A_CRT_VIDEORAM_SIZE, ENDIANNESS_LITTLE)
		, m_io_y(*this, "Y.%u", 0)
	{ }

	void pc8500(machine_config &config);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;

private:
	required_device<cpu_device> m_maincpu;
	required_device<upd1990a_device> m_rtc;
	required_device<sed1330_device> m_lcdc;
	required_device<screen_device> m_screen;
	required_device<generic_slot_device> m_cart;
	required_device<generic_slot_device> m_io_cart;
	required_device<ram_device> m_ram;
	required_memory_region m_rom;
	memory_share_creator<uint8_t> m_crt_ram;
	required_ioport_array<10> m_io_y;

	memory_region *m_cart_rom = nullptr;

	void mmr_w(uint8_t data);
	uint8_t mmr_r();
	uint8_t rtc_r();
	void rtc_cmd_w(uint8_t data);
	void rtc_ctrl_w(uint8_t data);
	uint8_t io_rom_data_r();
	void io_rom_addr_w(offs_t offset, uint8_t data);
	uint8_t port70_r();
	uint8_t port71_r();
	void port70_w(uint8_t data);
	void port71_w(uint8_t data);
	void palette_init(palette_device &palette) const;

	void scan_keyboard();
	void bankswitch(uint8_t data);

	// keyboard state
	int m_key_strobe = 0;           // key pressed

	// memory state
	uint8_t m_mmr = 0;                // memory mapping register
	uint32_t m_io_addr = 0;           // I/O ROM address counter

	uint8_t m_key_latch = 0;
	bool m_key_irq_enable = false;
	TIMER_DEVICE_CALLBACK_MEMBER(keyboard_tick);
	[[maybe_unused]] void pc8401a_lcdc(address_map &map);
	void pc8401a_mem(address_map &map);
	void pc8500_io(address_map &map);
	void pc8500_lcdc(address_map &map);
};


void pc8401a_state::palette_init(palette_device &palette) const
{
	palette.set_pen_color(0, rgb_t(39, 108, 51));
	palette.set_pen_color(1, rgb_t(16, 37, 84));
}

void pc8401a_state::pc8401a_lcdc(address_map &map)
{
	map.global_mask(0x1fff);
	map(0x0000, 0x1fff).ram();
}

void pc8401a_state::pc8500_lcdc(address_map &map)
{
	map.global_mask(0x3fff);
	map(0x0000, 0x3fff).ram();
}

void pc8401a_state::scan_keyboard()
{
	if (!m_key_irq_enable)
		return;
	int strobe = 0;

	/* scan keyboard */
	for (int row = 0; row < 10; row++)
	{
		uint8_t data = m_io_y[row]->read();

		if (data != 0xff)
		{
			strobe = 1;
			m_key_latch = data;
		}
	}

	if (!m_key_strobe && strobe)
	{
		m_maincpu->set_input_line_and_vector(INPUT_LINE_IRQ0, ASSERT_LINE, 0xef); // Z80 - RST 28h
	}

	if (strobe) m_key_strobe = strobe;
}

TIMER_DEVICE_CALLBACK_MEMBER(pc8401a_state::keyboard_tick)
{
	scan_keyboard();
}

/*
 *
 * bit     description
 * 0       key pressed
 * 1
 * 2
 * 3
 * 4       must be 1 or CPU goes to HALT (power switch status?)
 * 5
 * 6
 * 7
 *
 */
uint8_t pc8401a_state::port70_r()
{
	return 0x10 | (m_key_strobe & 1);
}

uint8_t pc8401a_state::port71_r()
{
	return m_key_latch;
}

void pc8401a_state::port70_w(uint8_t data)
{
	m_key_strobe = 0;
}

void pc8401a_state::port71_w(uint8_t data)
{
	m_maincpu->set_input_line(INPUT_LINE_IRQ0, CLEAR_LINE);

	// guess: machine starts with a 0x10 -> 0x18 transition -> ei
	if (data == 0x18 && m_key_latch == 0x10)
		m_key_irq_enable = true;
	m_key_latch = data;
}

void pc8401a_state::bankswitch(uint8_t data)
{
	address_space &program = m_maincpu->space(AS_PROGRAM);

	int rombank = data & 0x03;
	int ram0000 = (data >> 2) & 0x03;
	int ram8000 = (data >> 4) & 0x03;

	switch (ram0000)
	{
	case 0: /* ROM 0000H to 7FFFH */
		if (rombank < 3)
		{
			/* internal ROM */
			program.install_read_bank(0x0000, 0x7fff, membank("bank1"));
			program.unmap_write(0x0000, 0x7fff);
			membank("bank1")->set_entry(rombank);
		}
		else if (m_cart_rom)
		{
			/* ROM cartridge */
			program.install_read_bank(0x0000, 0x7fff, membank("bank1"));
			program.unmap_write(0x0000, 0x7fff);
			membank("bank1")->set_entry(6);
		}
		else
			program.unmap_readwrite(0x0000, 0x7fff);
		//logerror("0x0000-0x7fff = ROM %u\n", rombank);
		break;

	case 1: /* RAM 0000H to 7FFFH */
		program.install_readwrite_bank(0x0000, 0x7fff, membank("bank1"));
		membank("bank1")->set_entry(4);
		//logerror("0x0000-0x7fff = RAM 0-7fff\n");
		break;

	case 2: /* RAM 8000H to FFFFH */
		program.install_readwrite_bank(0x0000, 0x7fff, membank("bank1"));
		membank("bank1")->set_entry(5);
		//logerror("0x0000-0x7fff = RAM 8000-ffff\n");
		break;

	case 3: /* invalid */
		logerror("0x0000-0x7fff = invalid\n");
		break;
	}

	switch (ram8000)
	{
	case 0: /* cell addresses 0000H to 3FFFH */
		program.install_readwrite_bank(0x8000, 0xbfff, membank("bank3"));
		membank("bank3")->set_entry(0);
		//logerror("0x8000-0xbfff = RAM 0-3fff\n");
		break;

	case 1: /* cell addresses 4000H to 7FFFH */
		program.install_readwrite_bank(0x8000, 0xbfff, membank("bank3"));
		membank("bank3")->set_entry(1);
		//logerror("0x8000-0xbfff = RAM 4000-7fff\n");
		break;

	case 2: /* cell addresses 8000H to BFFFH */
		program.install_readwrite_bank(0x8000, 0xbfff, membank("bank3"));
		membank("bank3")->set_entry(2);
		//logerror("0x8000-0xbfff = RAM 8000-bfff\n");
		break;

	case 3: /* RAM cartridge */
		if (m_ram->size() > 64)
		{
			program.install_readwrite_bank(0x8000, 0xbfff, membank("bank3"));
			membank("bank3")->set_entry(3); // TODO or 4
		}
		else
		{
			program.unmap_readwrite(0x8000, 0xbfff);
		}
		//logerror("0x8000-0xbfff = RAM cartridge\n");
		break;
	}

	if (BIT(data, 6))
	{
		/* CRT video RAM */
		program.install_readwrite_bank(0xc000, 0xdfff, membank("bank4"));
		program.unmap_readwrite(0xe000, 0xe7ff);
		membank("bank4")->set_entry(1);
		//logerror("0xc000-0xdfff = video RAM\n");
	}
	else
	{
		/* RAM */
		program.install_readwrite_bank(0xc000, 0xe7ff, membank("bank4"));
		membank("bank4")->set_entry(0);
		//logerror("0xc000-0e7fff = RAM c000-e7fff\n");
	}
}

/*
 *
 * bit     description
 * 0       ROM section bit 0
 * 1       ROM section bit 1
 * 2       mapping for CPU addresses 0000H to 7FFFH bit 0
 * 3       mapping for CPU addresses 0000H to 7FFFH bit 1
 * 4       mapping for CPU addresses 8000H to BFFFH bit 0
 * 5       mapping for CPU addresses 8000H to BFFFH bit 1
 * 6       mapping for CPU addresses C000H to E7FFH
 * 7
 */
void pc8401a_state::mmr_w(uint8_t data)
{
	if (data != m_mmr)
	{
		bankswitch(data);
	}

	m_mmr = data;
}

uint8_t pc8401a_state::mmr_r()
{
	return m_mmr;
}

/*
 * bit     description
 * 0       RTC TP?
 * 1       RTC DATA OUT
 * 2       ?
 * 3
 * 4
 * 5
 * 6
 * 7
 */
uint8_t pc8401a_state::rtc_r()
{
	return (m_rtc->data_out_r() << 1) | (m_rtc->tp_r() << 2);
}

// Virtually same as pc8001_state::port10_w
void pc8401a_state::rtc_cmd_w(uint8_t data)
{
	m_rtc->c0_w(BIT(data, 0));
	m_rtc->c1_w(BIT(data, 1));
	m_rtc->c2_w(BIT(data, 2));
	m_rtc->data_in_w(BIT(data, 3));

	// TODO: centronics port?
}

/*
 * ---- -x-- RTC CLK
 * ---- --x- RTC STB
 * ---- ---x RTC OE?
 */
void pc8401a_state::rtc_ctrl_w(uint8_t data)
{
	m_rtc->oe_w(BIT(data, 0));
	m_rtc->stb_w(BIT(data, 1));
	m_rtc->clk_w(BIT(data, 2));
}

uint8_t pc8401a_state::io_rom_data_r()
{
	//logerror("I/O ROM read from %05x\n", m_io_addr);
	return m_io_cart->read_rom(m_io_addr);
}

void pc8401a_state::io_rom_addr_w(offs_t offset, uint8_t data)
{
	switch (offset)
	{
	case 0: /* A17..A16 */
		m_io_addr = ((data & 0x03) << 16) | (m_io_addr & 0xffff);
		break;

	case 1: /* A15..A8 */
		m_io_addr = (m_io_addr & 0x300ff) | (data << 8);
		break;

	case 2: /* A7..A0 */
		m_io_addr = (m_io_addr & 0x3ff00) | data;
		break;

	case 3:
		/* the same data is written here as to 0xb2, maybe this latches the address value? */
		break;
	}
}


void pc8401a_state::pc8401a_mem(address_map &map)
{
	map.unmap_value_high();
	map(0x0000, 0x7fff).bankrw("bank1");
	map(0x8000, 0xbfff).bankrw("bank3");
	map(0xc000, 0xe7ff).bankrw("bank4");
	map(0xe800, 0xffff).bankrw("bank5");
}

void pc8401a_state::pc8500_io(address_map &map)
{
	map.unmap_value_high();
	map.global_mask(0xff);
	map(0x00, 0x00).portr("Y.0");
	map(0x01, 0x01).portr("Y.1");
	map(0x02, 0x02).portr("Y.2");
	map(0x03, 0x03).portr("Y.3");
	map(0x04, 0x04).portr("Y.4");
	map(0x05, 0x05).portr("Y.5");
	map(0x06, 0x06).portr("Y.6");
	map(0x07, 0x07).portr("Y.7");
	map(0x08, 0x08).portr("Y.8");
	map(0x09, 0x09).portr("Y.9");
	map(0x10, 0x10).w(FUNC(pc8401a_state::rtc_cmd_w));
	map(0x20, 0x21).rw(I8251_TAG, FUNC(i8251_device::read), FUNC(i8251_device::write));
	map(0x30, 0x30).rw(FUNC(pc8401a_state::mmr_r), FUNC(pc8401a_state::mmr_w));
//  map(0x31, 0x31)
	map(0x40, 0x40).rw(FUNC(pc8401a_state::rtc_r), FUNC(pc8401a_state::rtc_ctrl_w));
//  map(0x41, 0x41)
//  map(0x50, 0x51)
	map(0x60, 0x60).rw(m_lcdc, FUNC(sed1330_device::status_r), FUNC(sed1330_device::data_w));
	map(0x61, 0x61).rw(m_lcdc, FUNC(sed1330_device::data_r), FUNC(sed1330_device::command_w));
	map(0x70, 0x70).rw(FUNC(pc8401a_state::port70_r), FUNC(pc8401a_state::port70_w));
	map(0x71, 0x71).rw(FUNC(pc8401a_state::port71_r), FUNC(pc8401a_state::port71_w));
//  map(0x80, 0x80) modem status, set to 0xff to boot
//  map(0x8b, 0x8b)
//  map(0x90, 0x93)
//	map(0x98, 0x98).w(m_crtc, FUNC(mc6845_device::address_w));
//	map(0x99, 0x99).rw(m_crtc, FUNC(mc6845_device::register_r), FUNC(mc6845_device::register_w));
	map(0x98, 0x99).noprw();
//  map(0xa0, 0xa1)
	map(0xb0, 0xb3).w(FUNC(pc8401a_state::io_rom_addr_w));
	map(0xb3, 0xb3).r(FUNC(pc8401a_state::io_rom_data_r));
//  map(0xc8, 0xc8)
//	map(0xfc, 0xff).rw(I8255A_TAG, FUNC(i8255_device::read), FUNC(i8255_device::write));
	map(0xfc, 0xff).noprw();
}

static INPUT_PORTS_START( pc8401a )
	PORT_START("Y.0")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("STOP")// PORT_CODE(KEYCODE_ESC) PORT_CHAR(UCHAR_MAMEKEY(ESC))
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("SHIFT") PORT_CODE(KEYCODE_LSHIFT) PORT_CODE(KEYCODE_RSHIFT) PORT_CHAR(UCHAR_SHIFT_1)
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD )

	PORT_START("Y.1")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_G) PORT_CHAR('g') PORT_CHAR('G')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F) PORT_CHAR('f') PORT_CHAR('F')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_E) PORT_CHAR('e') PORT_CHAR('E')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_D) PORT_CHAR('d') PORT_CHAR('D')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_C) PORT_CHAR('c') PORT_CHAR('C')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_B) PORT_CHAR('b') PORT_CHAR('B')
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_A) PORT_CHAR('a') PORT_CHAR('A')
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("SPACE") PORT_CODE(KEYCODE_SPACE) PORT_CHAR(' ')

	PORT_START("Y.2")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_O) PORT_CHAR('o') PORT_CHAR('O')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_N) PORT_CHAR('n') PORT_CHAR('N')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_M) PORT_CHAR('m') PORT_CHAR('M')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_L) PORT_CHAR('l') PORT_CHAR('L')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_K) PORT_CHAR('k') PORT_CHAR('K')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_J) PORT_CHAR('j') PORT_CHAR('J')
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_I) PORT_CHAR('i') PORT_CHAR('I')
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_H) PORT_CHAR('h') PORT_CHAR('H')

	PORT_START("Y.3")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_W) PORT_CHAR('w') PORT_CHAR('W')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_V) PORT_CHAR('v') PORT_CHAR('V')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_U) PORT_CHAR('u') PORT_CHAR('U')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_T) PORT_CHAR('t') PORT_CHAR('T')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_S) PORT_CHAR('s') PORT_CHAR('S')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_R) PORT_CHAR('r') PORT_CHAR('R')
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_Q) PORT_CHAR('q') PORT_CHAR('Q')
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_P) PORT_CHAR('p') PORT_CHAR('P')

	PORT_START("Y.4")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_MINUS) PORT_CHAR('-') PORT_CHAR('*')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_QUOTE) PORT_CHAR('\'') PORT_CHAR('*')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_CLOSEBRACE) PORT_CHAR(']') PORT_CHAR('*')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_BACKSLASH) PORT_CHAR('\\') PORT_CHAR('*')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_OPENBRACE) PORT_CHAR('[') PORT_CHAR('*')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_Z) PORT_CHAR('z') PORT_CHAR('Z')
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_Y) PORT_CHAR('y') PORT_CHAR('Y')
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_X) PORT_CHAR('x') PORT_CHAR('X')

	PORT_START("Y.5")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_7) PORT_CHAR('7') PORT_CHAR('*')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_6) PORT_CHAR('6') PORT_CHAR('*')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_5) PORT_CHAR('5') PORT_CHAR('*')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_4) PORT_CHAR('4') PORT_CHAR('*')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_3) PORT_CHAR('3') PORT_CHAR('*')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_2) PORT_CHAR('2') PORT_CHAR('*')
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_1) PORT_CHAR('1') PORT_CHAR('*')
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_0) PORT_CHAR('0') PORT_CHAR('*')

	PORT_START("Y.6")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_EQUALS) PORT_CHAR('=') PORT_CHAR('*')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_SLASH) PORT_CHAR('/') PORT_CHAR('*')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_STOP) PORT_CHAR('.') PORT_CHAR('*')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_COMMA) PORT_CHAR(',') PORT_CHAR('<')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) // ?
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_COLON) PORT_CHAR(';') PORT_CHAR('*')
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_9) PORT_CHAR('9') PORT_CHAR('*')
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_8) PORT_CHAR('8') PORT_CHAR('*')

	PORT_START("Y.7")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("ESC") PORT_CODE(KEYCODE_ESC) PORT_CHAR(UCHAR_MAMEKEY(ESC))
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) // ^I
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("F5") PORT_CODE(KEYCODE_F5) PORT_CHAR(UCHAR_MAMEKEY(F5))
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("F4") PORT_CODE(KEYCODE_F4) PORT_CHAR(UCHAR_MAMEKEY(F4))
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("F3") PORT_CODE(KEYCODE_F3) PORT_CHAR(UCHAR_MAMEKEY(F3))
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("F2") PORT_CODE(KEYCODE_F2) PORT_CHAR(UCHAR_MAMEKEY(F2))
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("F1") PORT_CODE(KEYCODE_F1) PORT_CHAR(UCHAR_MAMEKEY(F1))
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) // ^C

	PORT_START("Y.8")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(UTF8_RIGHT) PORT_CODE(KEYCODE_RIGHT) PORT_CHAR(UCHAR_MAMEKEY(RIGHT))
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F6)
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F7)
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_UNUSED )
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_UNUSED )
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_UNUSED )
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_UNUSED )
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("Y.9")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F8)
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F9)
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F10)
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F11)
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F12)
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("ENTER") PORT_CODE(KEYCODE_ENTER) PORT_CHAR(13)
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("DEL BKSP") PORT_CODE(KEYCODE_BACKSPACE) PORT_CHAR(8)
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(UTF8_LEFT) PORT_CODE(KEYCODE_LEFT) PORT_CHAR(UCHAR_MAMEKEY(LEFT))
INPUT_PORTS_END

void pc8401a_state::machine_start()
{
	std::string region_tag;
	m_cart_rom = memregion(region_tag.assign(m_cart->tag()).append(GENERIC_ROM_REGION_TAG).c_str());

	/* initialize RTC */
	m_rtc->cs_w(1);

	uint8_t *ram = m_ram->pointer();

	/* set up A0/A1 memory banking */
	membank("bank1")->configure_entries(0, 4, m_rom->base(), 0x8000);
	membank("bank1")->configure_entries(4, 2, ram, 0x8000);
	if (m_cart_rom)
		membank("bank1")->configure_entries(6, 1, m_cart_rom->base(), 0x8000);
	membank("bank1")->set_entry(0);

	/* set up A2 memory banking */
	membank("bank3")->configure_entries(0, 5, ram, 0x4000);
	membank("bank3")->set_entry(0);

	/* set up A3 memory banking */
	membank("bank4")->configure_entry(0, ram + 0xc000);
	membank("bank4")->configure_entry(1, m_crt_ram);
	membank("bank4")->set_entry(0);

	/* set up A4 memory banking */
	membank("bank5")->configure_entry(0, ram + 0xe800);
	membank("bank5")->set_entry(0);

	/* bank switch */
	bankswitch(0);

	/* register for state saving */
	save_item(NAME(m_mmr));
	save_item(NAME(m_io_addr));
}

void pc8401a_state::machine_reset()
{
	m_key_irq_enable = false;
}

void pc8401a_state::pc8500(machine_config &config)
{
	Z80(config, m_maincpu, 7.987_MHz_XTAL / 2); // NEC uPD70008C
	m_maincpu->set_addrmap(AS_PROGRAM, &pc8401a_state::pc8401a_mem);
	m_maincpu->set_addrmap(AS_IO, &pc8401a_state::pc8500_io);

	TIMER(config, "keyboard").configure_periodic(FUNC(pc8401a_state::keyboard_tick), attotime::from_hz(44));

	UPD1990A(config, m_rtc);

	i8251_device &uart(I8251(config, I8251_TAG, 0));
	uart.txd_handler().set(RS232_TAG, FUNC(rs232_port_device::write_txd));
	uart.dtr_handler().set(RS232_TAG, FUNC(rs232_port_device::write_dtr));
	uart.rts_handler().set(RS232_TAG, FUNC(rs232_port_device::write_rts));

	rs232_port_device &rs232(RS232_PORT(config, RS232_TAG, default_rs232_devices, nullptr));
	rs232.rxd_handler().set(I8251_TAG, FUNC(i8251_device::write_rxd));
	rs232.dsr_handler().set(I8251_TAG, FUNC(i8251_device::write_dsr));

	PALETTE(config, "palette", FUNC(pc8401a_state::palette_init), 2 + 8);

	// pc8401a uses 128 display lines
	SCREEN(config, m_screen, SCREEN_TYPE_LCD);
	m_screen->set_refresh_hz(44);
	m_screen->set_screen_update(SED1330_TAG, FUNC(sed1330_device::screen_update));
	m_screen->set_size(480, 208);
	m_screen->set_visarea(0, 480-1, 0, 200-1);
	m_screen->set_palette("palette");

	SED1330(config, m_lcdc, 7.987_MHz_XTAL);
	m_lcdc->set_screen(SCREEN_TAG);
	m_lcdc->set_addrmap(0, &pc8401a_state::pc8500_lcdc);

	/* option ROM cartridge */
	GENERIC_CARTSLOT(config, m_cart, generic_plain_slot, nullptr, "bin,rom");

	/* I/O ROM cartridge */
	GENERIC_CARTSLOT(config, m_io_cart, generic_linear_slot, nullptr, "bin,rom");

	RAM(config, RAM_TAG).set_default_size("64K").set_extra_options("96K");
}

ROM_START( pc8500 )
	ROM_REGION( 0x20000, Z80_TAG, ROMREGION_ERASEFF )
	ROM_LOAD( "pc8500.bin", 0x0000, 0x10000, CRC(c2749ef0) SHA1(f766afce9fda9ec84ed5b39ebec334806798afb3) )

	// TODO: identify this
	ROM_REGION( 0x1000, "mcu", 0)
	ROM_LOAD( "kbd.rom", 0x0000, 0x1000, NO_DUMP )

	//ROM_REGION( 0x1000, "chargen", 0 )
	//ROM_LOAD( "pc8441a.bin", 0x0000, 0x1000, NO_DUMP )
ROM_END

} // anonymous namespace

/* System Drivers */

/*    YEAR  NAME      PARENT   COMPAT  MACHINE  INPUT    CLASS          INIT        COMPANY  FULLNAME */
//COMP( 1984, pc8401a,  0,       0,      pc8401a, pc8401a, pc8401a_state, empty_init, "NEC",   "PC-8401A-LS", MACHINE_NOT_WORKING | MACHINE_NO_SOUND)
//COMP( 1984, pc8401bd, pc8401a, 0,      pc8401a, pc8401a, pc8401a_state, empty_init, "NEC",   "PC-8401BD", MACHINE_NOT_WORKING)
COMP( 1985, pc8500,   0,       0,      pc8500,  pc8401a, pc8401a_state,  empty_init, "NEC",   "PC-8500", MACHINE_NOT_WORKING | MACHINE_NO_SOUND)
