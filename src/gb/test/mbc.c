/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/gb/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/mbc.h>
#include <mgba-util/vfs.h>

M_TEST_SUITE_SETUP(GBMBC) {
	struct VFile* vf = VFileMemChunk(NULL, 2048);
	GBSynthesizeROM(vf);
	struct mCore* core = GBCoreCreate();
	core->init(core);
	mCoreInitConfig(core, NULL);
	core->loadROM(core, vf);
	*state = core;
	return 0;
}

M_TEST_SUITE_TEARDOWN(GBMBC) {
	if (!*state) {
		return 0;
	}
	struct mCore* core = *state;
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return 0;
}
M_TEST_DEFINE(detectNone) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x00;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC_NONE);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x08;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC_NONE);


	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x09;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC_NONE);


	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x0A;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC_NONE);
}

M_TEST_DEFINE(detect1) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x00;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC1);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x01;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC1);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x02;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC1);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x03;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC1);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x04;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC1);
}

M_TEST_DEFINE(detect2) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x04;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC2);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x05;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC2);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x06;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC2);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x07;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC2);
}

M_TEST_DEFINE(detect3) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x0E;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC3);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC3_RTC);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x0F;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC3_RTC);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x10;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC3_RTC);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x11;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC3);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x12;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC3);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x13;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC3);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x14;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC3);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC3_RTC);
}

M_TEST_DEFINE(detect5) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x19;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC5);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x1A;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC5);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x1B;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC5);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x1C;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC5_RUMBLE);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x1D;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC5_RUMBLE);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x1E;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC5_RUMBLE);
}

M_TEST_DEFINE(detect6) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x1F;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC6);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x20;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC6);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x21;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC6);
}

M_TEST_DEFINE(detect7) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x21;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC7);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x22;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC7);

	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x23;
	core->reset(core);
	assert_int_not_equal(gb->memory.mbcType, GB_MBC7);
}

/* Synthetic emulator fixture: these transitions do not assert hardware behavior. */
static void _mbc6EraseFixture(struct mCore* core) {
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];
	gb->memory.mbcType = GB_MBC_AUTODETECT;
	cart->type = 0x20;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC6);
	assert_non_null(gb->memory.sram);
	assert_true(gb->memory.sramSize >= GB_SIZE_MBC6_FLASH_STORAGE);
	uint8_t* flash = &gb->memory.sram[gb->memory.sramSize - GB_SIZE_MBC6_FLASH_STORAGE];
	memset(flash, 0xFF, GB_SIZE_MBC6_FLASH_STORAGE);
	flash[0x60 * GB_SIZE_CART_HALFBANK] = 0xA5;
	flash[0x70 * GB_SIZE_CART_HALFBANK] = 0x5A;

	core->busWrite8(core, 0x3800, 0x08);
	core->busWrite8(core, 0x0C00, 1);
	core->busWrite8(core, 0x1000, 1);
	core->busWrite8(core, 0x3000, 2);
	core->busWrite8(core, 0x7555, 0xAA);
	core->busWrite8(core, 0x3000, 1);
	core->busWrite8(core, 0x6AAA, 0x55);
	core->busWrite8(core, 0x3000, 2);
	core->busWrite8(core, 0x7555, 0x80);
	core->busWrite8(core, 0x7555, 0xAA);
	core->busWrite8(core, 0x3000, 1);
	core->busWrite8(core, 0x6AAA, 0x55);
	core->busWrite8(core, 0x3000, 0x70);
	core->busWrite8(core, 0x6000, 0x30);
	assert_true(gb->memory.mbcState.mbc6.flashOperationBusy);
	assert_true(gb->memory.mbcState.mbc6.flashIoBankValid);
	assert_int_equal(gb->memory.mbcState.mbc6.flashIoBank, 0x70);
}

static void _mbc6CompleteEraseFixture(struct GB* gb) {
	struct mTimingEvent* event = &gb->memory.mbc6FlashEvent;
	mTimingDeschedule(&gb->timing, event);
	event->callback(&gb->timing, event->context, 0);
	assert_false(gb->memory.mbcState.mbc6.flashOperationBusy);
	assert_true(gb->memory.mbcState.mbc6.flashIoBankValid);
}

M_TEST_DEFINE(mbc6EraseLatchContinuousAccess) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	_mbc6EraseFixture(core);
	_mbc6CompleteEraseFixture(gb);
	core->busWrite8(core, 0x6000, 0xF0);
	core->busWrite8(core, 0x3000, 0x60);
	assert_true(gb->memory.mbcState.mbc6.flashIoBankValid);
	assert_int_equal(core->busRead8(core, 0x6000), 0xFF);
}

M_TEST_DEFINE(mbc6EraseLatchArrayAccessCycle) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	_mbc6EraseFixture(core);
	_mbc6CompleteEraseFixture(gb);
	core->busWrite8(core, 0x6000, 0xF0);
	core->busWrite8(core, 0x0C00, 0);
	assert_false(gb->memory.mbcState.mbc6.flashIoBankValid);
	assert_int_equal(core->busRead8(core, 0x6000), 0xFF);
	core->busWrite8(core, 0x3000, 0x60);
	core->busWrite8(core, 0x0C00, 1);
	assert_int_equal(core->busRead8(core, 0x6000), 0xA5);
}

M_TEST_DEFINE(mbc6EraseLatchBusyAccessCycle) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	_mbc6EraseFixture(core);
	core->busWrite8(core, 0x0C00, 0);
	assert_true(gb->memory.mbcState.mbc6.flashIoBankValid);
	core->busWrite8(core, 0x3000, 0x60);
	core->busWrite8(core, 0x0C00, 1);
	assert_int_equal(core->busRead8(core, 0x6000), 0);
	_mbc6CompleteEraseFixture(gb);
	core->busWrite8(core, 0x6000, 0xF0);
	assert_int_equal(core->busRead8(core, 0x6000), 0xFF);
}

M_TEST_DEFINE(mbc6EraseLatchStatusAccessCycle) {
	struct mCore* core = *state;
	struct GB* gb = core->board;
	_mbc6EraseFixture(core);
	_mbc6CompleteEraseFixture(gb);
	core->busWrite8(core, 0x0C00, 0);
	assert_true(gb->memory.mbcState.mbc6.flashIoBankValid);
	core->busWrite8(core, 0x3000, 0x60);
	core->busWrite8(core, 0x0C00, 1);
	assert_int_equal(core->busRead8(core, 0x6000), 0x80);
	core->busWrite8(core, 0x6000, 0xF0);
	assert_int_equal(core->busRead8(core, 0x6000), 0xFF);
}

/* Check the emulator's existing wrap policy, not the electrical meaning of bit 7. */
static void _mbc6ROMZeroFixture(struct mCore* core, uint8_t selector) {
	struct VFile* vf = VFileMemChunk(NULL, GB_SIZE_MBC6_FLASH);
	assert_non_null(vf);
	GBSynthesizeROM(vf);
	assert_true(core->loadROM(core, vf));
	struct GB* gb = core->board;
	struct GBCartridge* cart = (struct GBCartridge*) &gb->memory.rom[0x100];
	cart->type = 0x20;
	gb->memory.mbcType = GB_MBC_AUTODETECT;
	core->reset(core);
	assert_int_equal(gb->memory.mbcType, GB_MBC6);
	assert_int_equal(gb->memory.romSize, GB_SIZE_MBC6_FLASH);
	gb->memory.rom[0] = 0x11;
	gb->memory.rom[GB_SIZE_CART_HALFBANK] = 0x22;
	uint8_t* flash = &gb->memory.sram[gb->memory.sramSize - GB_SIZE_MBC6_FLASH_STORAGE];
	flash[0] = 0xCC;
	core->busWrite8(core, 0x0C00, 1);

	for (unsigned half = 0; half < 2; ++half) {
		uint16_t bankRegister = half ? 0x3000 : 0x2000;
		uint16_t sourceRegister = half ? 0x3800 : 0x2800;
		uint16_t window = half ? 0x6000 : 0x4000;
		core->busWrite8(core, sourceRegister, 0);
		core->busWrite8(core, bankRegister, selector);
		assert_int_equal(half ? gb->memory.currentBank1 : gb->memory.currentBank, 0);
		assert_ptr_equal(half ? gb->memory.romBank1 : gb->memory.romBank, gb->memory.rom);
		assert_int_equal(core->busRead8(core, window), 0x11);

		core->busWrite8(core, sourceRegister, 8);
		assert_int_equal(half ? gb->memory.currentBank1 : gb->memory.currentBank, 0);
		assert_ptr_equal(half ? gb->memory.romBank1 : gb->memory.romBank, flash);
		assert_int_equal(core->busRead8(core, window), 0xCC);

		core->busWrite8(core, sourceRegister, 0);
		assert_int_equal(half ? gb->memory.currentBank1 : gb->memory.currentBank, 0);
		assert_ptr_equal(half ? gb->memory.romBank1 : gb->memory.romBank, gb->memory.rom);
		assert_int_equal(core->busRead8(core, window), 0x11);
	}
}

M_TEST_DEFINE(mbc6ROMZeroMapping) {
	_mbc6ROMZeroFixture(*state, 0);
}

M_TEST_DEFINE(mbc6ROMWrappedZeroMapping) {
	_mbc6ROMZeroFixture(*state, 0x80);
}

M_TEST_SUITE_DEFINE_SETUP_TEARDOWN(GBMBC,
	cmocka_unit_test(detectNone),
	cmocka_unit_test(detect1),
	cmocka_unit_test(detect2),
	cmocka_unit_test(detect3),
	cmocka_unit_test(detect5),
	cmocka_unit_test(detect6),
	cmocka_unit_test(detect7),
	cmocka_unit_test(mbc6EraseLatchContinuousAccess),
	cmocka_unit_test(mbc6EraseLatchArrayAccessCycle),
	cmocka_unit_test(mbc6EraseLatchBusyAccessCycle),
	cmocka_unit_test(mbc6EraseLatchStatusAccessCycle),
	cmocka_unit_test(mbc6ROMZeroMapping),
	cmocka_unit_test(mbc6ROMWrappedZeroMapping))
