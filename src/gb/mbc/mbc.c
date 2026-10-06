/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gb/mbc/mbc-private.h"

#include <mgba/core/interface.h>
#include <mgba/internal/defines.h>
#include <mgba/internal/gb/gb.h>

static void _GBMBC6MapChip(struct GB*, int half, uint8_t value);
static uint32_t _GBMBC6FlashOffset(const struct GBMemory*, uint16_t);
static uint32_t _GBMBC6FlashArrayOffset(const struct GBMemory*, uint16_t);
static void _GBMBC6FlashWrite(struct GB*, uint16_t, uint32_t, uint8_t);
static void _GBMBC6FlashComplete(struct mTiming*, void*, uint32_t);

void _GBMBC6InitFlashEvent(struct GB* gb) {
	gb->memory.mbc6FlashEvent.context = gb;
	gb->memory.mbc6FlashEvent.name = "GB MBC6 flash operation";
	gb->memory.mbc6FlashEvent.callback = _GBMBC6FlashComplete;
	gb->memory.mbc6FlashEvent.priority = 0x42;
}

#define GB_MBC6_FLASH_BUSY_CYCLES 25166 /* Iceboy measured the longest Net de Get operation at about 6 ms. */

enum {
	GB_MBC6_FLASH_OP_NONE,
	GB_MBC6_FLASH_OP_PROGRAM,
	GB_MBC6_FLASH_OP_ERASE_SECTOR,
	GB_MBC6_FLASH_OP_ERASE_CHIP,
	GB_MBC6_FLASH_OP_ERASE_HIDDEN,
	GB_MBC6_FLASH_OP_PROTECT,
	GB_MBC6_FLASH_OP_UNPROTECT,
};

void _GBMBCLatchRTC(struct mRTCSource* rtc, uint8_t* rtcRegs, time_t* rtcLastLatch) {
	time_t t;
	if (rtc) {
		if (rtc->sample) {
			rtc->sample(rtc);
		}
		t = rtc->unixTime(rtc);
	} else {
		t = time(0);
	}
	time_t currentLatch = t;
	t -= *rtcLastLatch;
	*rtcLastLatch = currentLatch;

	int64_t diff;
	diff = rtcRegs[0] + t % 60;
	if (diff < 0) {
		diff += 60;
		t -= 60;
	}
	rtcRegs[0] = diff % 60;
	t /= 60;
	t += diff / 60;

	diff = rtcRegs[1] + t % 60;
	if (diff < 0) {
		diff += 60;
		t -= 60;
	}
	rtcRegs[1] = diff % 60;
	t /= 60;
	t += diff / 60;

	diff = rtcRegs[2] + t % 24;
	if (diff < 0) {
		diff += 24;
		t -= 24;
	}
	rtcRegs[2] = diff % 24;
	t /= 24;
	t += diff / 24;

	diff = rtcRegs[3] + ((rtcRegs[4] & 1) << 8) + (t & 0x1FF);
	rtcRegs[3] = diff;
	rtcRegs[4] &= 0xFE;
	rtcRegs[4] |= (diff >> 8) & 1;
	if (diff & 0x200) {
		rtcRegs[4] |= 0x80;
	}
}

static void _GBMBC1Update(struct GB* gb) {
	struct GBMBC1State* state = &gb->memory.mbcState.mbc1;
	int bank = state->bankLo;
	bank &= (1 << state->multicartStride) - 1;
	bank |= state->bankHi << state->multicartStride;
	if (state->mode) {
		GBMBCSwitchBank0(gb, state->bankHi << state->multicartStride);
		GBMBCSwitchSramBank(gb, state->bankHi & 3);
	} else {
		GBMBCSwitchBank0(gb, 0);
		GBMBCSwitchSramBank(gb, 0);
	}
	if (!(state->bankLo & 0x1F)) {
		++state->bankLo;
		++bank;
	}
	GBMBCSwitchBank(gb, bank);
}

void _GBMBC1(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBMemory* memory = &gb->memory;
	int bank = value & 0x1F;
	switch (address >> 13) {
	case 0x0:
		switch (value & 0xF) {
		case 0:
			memory->sramAccess = false;
			break;
		case 0xA:
			memory->sramAccess = true;
			GBMBCSwitchSramBank(gb, memory->sramCurrentBank);
			break;
		default:
			// TODO
			mLOG(GB_MBC, STUB, "MBC1 unknown value %02X", value);
			break;
		}
		break;
	case 0x1:
		memory->mbcState.mbc1.bankLo = bank;
		_GBMBC1Update(gb);
		break;
	case 0x2:
		bank &= 3;
		memory->mbcState.mbc1.bankHi = bank;
		_GBMBC1Update(gb);
		break;
	case 0x3:
		memory->mbcState.mbc1.mode = value & 1;
		_GBMBC1Update(gb);
		break;
	default:
		// TODO
		mLOG(GB_MBC, STUB, "MBC1 unknown address: %04X:%02X", address, value);
		break;
	}
}

void _GBMBC2(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBMemory* memory = &gb->memory;
	int shift = (address & 1) * 4;
	int bank = value & 0xF;
	switch ((address & 0xC100) >> 8) {
	case 0x0:
		switch (value & 0x0F) {
		case 0:
			memory->sramAccess = false;
			break;
		case 0xA:
			memory->sramAccess = true;
			break;
		default:
			// TODO
			mLOG(GB_MBC, STUB, "MBC2 unknown value %02X", value);
			break;
		}
		break;
	case 0x1:
		if (!bank) {
			++bank;
		}
		GBMBCSwitchBank(gb, bank);
		break;
	case 0x80:
	case 0x81:
	case 0x82:
	case 0x83:
		if (!memory->sramAccess) {
			return;
		}
		address &= 0x1FF;
		memory->sramBank[(address >> 1)] &= 0xF0 >> shift;
		memory->sramBank[(address >> 1)] |= (value & 0xF) << shift;
		gb->sramDirty |= mSAVEDATA_DIRT_NEW;
		break;
	default:
		// TODO
		mLOG(GB_MBC, STUB, "MBC2 unknown address: %04X:%02X", address, value);
		break;
	}
}

uint8_t _GBMBC2Read(struct GBMemory* memory, uint16_t address) {
	if (!memory->sramAccess) {
		return 0xFF;
	}
	address &= 0x1FF;
	int shift = (address & 1) * 4;
	return (memory->sramBank[(address >> 1)] >> shift) | 0xF0;
}

void _GBMBC3(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBMemory* memory = &gb->memory;
	int bank = value;
	switch (address >> 13) {
	case 0x0:
		if ((value & 0xF) == 0xA) {
			memory->sramAccess = true;
			GBMBCSwitchSramBank(gb, memory->sramCurrentBank);
		} else {
			memory->sramAccess = false;
		}
		break;
	case 0x1:
		if (gb->memory.romSize < GB_SIZE_CART_BANK0 * 0x80) {
			bank &= 0x7F;
		}
		if (!bank) {
			++bank;
		}
		GBMBCSwitchBank(gb, bank);
		break;
	case 0x2:
		bank &= 0xF;
		if (bank < 8) {
			GBMBCSwitchSramBank(gb, value);
			memory->rtcAccess = false;
		} else if (bank <= 0xC) {
			memory->activeRtcReg = bank - 8;
			memory->rtcAccess = true;
		}
		break;
	case 0x3:
		if (memory->rtcLatched && value == 0) {
			memory->rtcLatched = false;
		} else if (!memory->rtcLatched && value == 1) {
			_GBMBCLatchRTC(gb->memory.rtc, gb->memory.rtcRegs, &gb->memory.rtcLastLatch);
			memory->rtcLatched = true;
		}
		break;
	}
}

void _GBMBC5(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBMemory* memory = &gb->memory;
	int bank;
	switch (address >> 12) {
	case 0x0:
	case 0x1:
		switch (value) {
		case 0:
			memory->sramAccess = false;
			break;
		case 0xA:
			memory->sramAccess = true;
			GBMBCSwitchSramBank(gb, memory->sramCurrentBank);
			break;
		default:
			// TODO
			mLOG(GB_MBC, STUB, "MBC5 unknown value %02X", value);
			break;
		}
		break;
	case 0x2:
		bank = (memory->currentBank & 0x100) | value;
		GBMBCSwitchBank(gb, bank);
		break;
	case 0x3:
		bank = (memory->currentBank & 0xFF) | ((value & 1) << 8);
		GBMBCSwitchBank(gb, bank);
		break;
	case 0x4:
	case 0x5:
		if (memory->mbcType == GB_MBC5_RUMBLE && memory->rumble) {
			int32_t currentTime = mTimingCurrentTime(&gb->timing);
			memory->rumble->setRumble(memory->rumble, (value >> 3) & 1, currentTime - memory->lastRumble);
			memory->lastRumble = currentTime;
			value &= ~8;
		}
		GBMBCSwitchSramBank(gb, value & 0xF);
		break;
	default:
		// TODO
		mLOG(GB_MBC, STUB, "MBC5 unknown address: %04X:%02X", address, value);
		break;
	}
}

void _GBMBC6(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBMemory* memory = &gb->memory;
	struct GBMBC6State* state = &memory->mbcState.mbc6;
	int bank = value;
	switch (address >> 10) {
	case 0:
		switch (value) {
		case 0:
			memory->sramAccess = false;
			break;
		case 0xA:
			memory->sramAccess = true;
			break;
		default:
			// TODO
			mLOG(GB_MBC, STUB, "MBC6 unknown value %02X", value);
			break;
		}
		break;
	case 0x1:
		GBMBCSwitchSramHalfBank(gb, 0, bank);
		break;
	case 0x2:
		GBMBCSwitchSramHalfBank(gb, 1, bank);
		break;
	case 0x3:
		state->flashEnable = value & 1;
		/* Net de Get ends an erase access session after F0 by disabling
		 * flash, then re-enables it to read a different bank's header.
		 * Keep the erase bank latched while that access session is open. */
		if (!state->flashEnable && !state->flashMode && !state->flashOperationBusy) {
			state->flashIoBankValid = false;
		}
		break;
	case 0x4:
		state->flashWriteEnable = value & 1;
		break;
	case 0x8:
	case 0x9:
		GBMBCSwitchHalfBank(gb, 0, bank);
		break;
	case 0xA:
	case 0xB:
		_GBMBC6MapChip(gb, 0, value);
		break;
	case 0xC:
	case 0xD:
		GBMBCSwitchHalfBank(gb, 1, bank);
		break;
	case 0xE:
	case 0xF:
		_GBMBC6MapChip(gb, 1, value);
		break;
	case 0x28:
	case 0x29:
	case 0x2A:
	case 0x2B:
		if (memory->sramAccess) {
			memory->sramBank[address & (GB_SIZE_EXTERNAL_RAM_HALFBANK - 1)] = value;
			gb->sramDirty |= mSAVEDATA_DIRT_NEW;
		}
		break;
	case 0x2C:
	case 0x2D:
	case 0x2E:
	case 0x2F:
		if (memory->sramAccess) {
			memory->sramBank1[address & (GB_SIZE_EXTERNAL_RAM_HALFBANK - 1)] = value;
			gb->sramDirty |= mSAVEDATA_DIRT_NEW;
		}
		break;
	default:
		if (address >= GB_BASE_CART_BANK1 && address < GB_BASE_VRAM &&
		    ((address < GB_BASE_CART_HALFBANK2 && state->flashBank0) || (address >= GB_BASE_CART_HALFBANK2 && state->flashBank1))) {
			if (state->flashEnable) {
				_GBMBC6FlashWrite(gb, address, _GBMBC6FlashOffset(memory, address), value);
			}
			break;
		}
		mLOG(GB_MBC, STUB, "MBC6 unknown address: %04X:%02X", address, value);
		break;
	}
}

uint8_t _GBMBC6Read(struct GBMemory* memory, uint16_t address) {
	struct GBMBC6State* state = &memory->mbcState.mbc6;
	if (address >= GB_BASE_CART_BANK1 && address < GB_BASE_VRAM) {
		uint8_t window = address >= GB_BASE_CART_HALFBANK2;
		bool operationWindow = state->flashOperationActive && state->flashOperationWindow == window;
		if ((address < GB_BASE_CART_HALFBANK2 && state->flashBank0) || (address >= GB_BASE_CART_HALFBANK2 && state->flashBank1)) {
			if (!state->flashEnable || !memory->sram) {
				return 0xFF;
			}
			uint32_t offset = _GBMBC6FlashOffset(memory, address);
			uint32_t arrayOffset = _GBMBC6FlashArrayOffset(memory, address);
			if (state->flashOperationBusy && state->flashMode >= 2) {
				return (state->flashSector0Protected ? 0x02 : 0);
			}
			if (state->flashOperationActive && state->flashOperationKind == GB_MBC6_FLASH_OP_ERASE_SECTOR &&
			    !operationWindow && state->flashMode >= 2) {
				return memory->sram[memory->sramSize - GB_SIZE_MBC6_FLASH_STORAGE + offset];
			}
			switch (state->flashMode) {
			case 1: // JEDEC autoselect
				switch (offset & 3) {
				case 0: return 0xC2;
				case 1: return 0x81;
				case 2: return offset < 0x20000 ? 0xC2 : 0x00;
				default: return 0xFF;
				}
			case 2: // Flash status
				return (state->flashOperationBusy && state->flashOperationWindow == window ? 0 : 0x80) | (state->flashSector0Protected ? 0x02 : 0);
			case 4: // Flash program buffer status
			case 5: // Hidden-region program buffer status
			case 6: // Erase command status
			case 7: // Sector 0 protection command status
				return 0x80 | (state->flashSector0Protected ? 0x02 : 0);
			case 3: // Hidden 256-byte region
				return memory->sram[memory->sramSize - GB_SIZE_MBC6_FLASH_STORAGE + GB_SIZE_MBC6_FLASH + (offset & 0xFF)];
			default:
				return memory->sram[memory->sramSize - GB_SIZE_MBC6_FLASH_STORAGE + arrayOffset];
			}
		}
		if (address < GB_BASE_CART_HALFBANK2) {
			return memory->romBank[address & (GB_SIZE_CART_HALFBANK - 1)];
		}
		return memory->romBank1[address & (GB_SIZE_CART_HALFBANK - 1)];
	}
	if (!memory->sramAccess) {
		return 0xFF;
	}
	switch (address >> 12) {
	case 0xA:
		return memory->sramBank[address & (GB_SIZE_EXTERNAL_RAM_HALFBANK - 1)];
	case 0xB:
		return memory->sramBank1[address & (GB_SIZE_EXTERNAL_RAM_HALFBANK - 1)];
	}
	return 0xFF;
}

enum {
	GB_MBC6_FLASH_CMD_IDLE,
	GB_MBC6_FLASH_CMD_UNLOCK_1,
	GB_MBC6_FLASH_CMD_UNLOCK_2,
	GB_MBC6_FLASH_CMD_ERASE_UNLOCK_1,
	GB_MBC6_FLASH_CMD_ERASE_UNLOCK_2,
	GB_MBC6_FLASH_CMD_ERASE_FINAL,
	GB_MBC6_FLASH_CMD_SPECIAL_UNLOCK_1,
	GB_MBC6_FLASH_CMD_SPECIAL_UNLOCK_2,
	GB_MBC6_FLASH_CMD_SPECIAL_FINAL,
	GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_1,
	GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_2,
	GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_3,
};

static uint32_t _GBMBC6FlashOffset(const struct GBMemory* memory, uint16_t address) {
	uint32_t bank = (uint32_t) (address < GB_BASE_CART_HALFBANK2 ? memory->currentBank : memory->currentBank1);
	return ((uint32_t) bank << 13) | (address & (GB_SIZE_CART_HALFBANK - 1));
}

static uint32_t _GBMBC6FlashArrayOffset(const struct GBMemory* memory, uint16_t address) {
	uint8_t window = address >= GB_BASE_CART_HALFBANK2;
	const struct GBMBC6State* state = &memory->mbcState.mbc6;
	if (state->flashIoBankValid && state->flashIoWindow == window) {
		return (state->flashIoBank << 13) | (address & (GB_SIZE_CART_HALFBANK - 1));
	}
	return _GBMBC6FlashOffset(memory, address);
}

static void _GBMBC6FlashDirty(struct GB* gb) {
	gb->sramDirty |= mSAVEDATA_DIRT_NEW;
}

static void _GBMBC6FlashStart(struct GB* gb, uint8_t kind, uint32_t target, uint8_t window, uint32_t bank) {
	struct GBMBC6State* state = &gb->memory.mbcState.mbc6;
	if (!gb->memory.sram || state->flashOperationBusy) {
		return;
	}
	state->flashOperationKind = kind;
	state->flashOperationTarget = target;
	state->flashOperationWindow = window;
	state->flashOperationBank = bank;
	state->flashOperationWriteEnable = state->flashWriteEnable;
	state->flashOperationSector0Protected = state->flashSector0Protected;
	if (kind == GB_MBC6_FLASH_OP_ERASE_SECTOR) {
		state->flashIoWindow = window;
		state->flashIoBank = bank;
		state->flashIoBankValid = true;
	}
	state->flashOperationActive = true;
	state->flashOperationBusy = true;
	state->flashMode = 2;
	state->flashProgramCount = 0;
	mTimingSchedule(&gb->timing, &gb->memory.mbc6FlashEvent, GB_MBC6_FLASH_BUSY_CYCLES);
}

static void _GBMBC6FlashComplete(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct GB* gb = context;
	struct GBMBC6State* state = &gb->memory.mbcState.mbc6;
	if (!state->flashOperationBusy || !gb->memory.sram) {
		return;
	}
	uint8_t* flash = &gb->memory.sram[gb->memory.sramSize - GB_SIZE_MBC6_FLASH_STORAGE];
	uint32_t target = state->flashOperationTarget;
	switch (state->flashOperationKind) {
	case GB_MBC6_FLASH_OP_PROGRAM:
		/* Flash write protection blocks sector 0 and the hidden region, not sectors 1-7. */
		if ((!state->flashOperationWriteEnable && target >= GB_SIZE_MBC6_FLASH) ||
		    (target < 0x20000 && (!state->flashOperationWriteEnable || state->flashOperationSector0Protected))) {
			break;
		}
		for (unsigned i = 0; i < 0x80; ++i) {
			if (state->flashProgramWritten[i >> 3] & (1 << (i & 7))) {
				flash[target + i] &= state->flashProgramBuffer[i];
			}
		}
		_GBMBC6FlashDirty(gb);
		break;
	case GB_MBC6_FLASH_OP_ERASE_SECTOR:
		/* Sectors 1-7 remain writable while flash write protection is enabled. */
		if (target >= 0x20000 || (state->flashOperationWriteEnable && !state->flashOperationSector0Protected)) {
			memset(&flash[target], 0xFF, 0x20000);
			_GBMBC6FlashDirty(gb);
		}
		break;
	case GB_MBC6_FLASH_OP_ERASE_CHIP:
		for (uint32_t sector = 0x20000; sector < GB_SIZE_MBC6_FLASH; sector += 0x20000) {
			memset(&flash[sector], 0xFF, 0x20000);
		}
		if (state->flashOperationWriteEnable && !state->flashOperationSector0Protected) {
			memset(flash, 0xFF, 0x20000);
		}
		_GBMBC6FlashDirty(gb);
		break;
	case GB_MBC6_FLASH_OP_ERASE_HIDDEN:
		if (state->flashOperationWriteEnable) {
			memset(&flash[GB_SIZE_MBC6_FLASH], 0xFF, 0x100);
			_GBMBC6FlashDirty(gb);
		}
		break;
	case GB_MBC6_FLASH_OP_PROTECT:
		if (state->flashOperationWriteEnable) {
			state->flashSector0Protected = true;
			gb->memory.sram[gb->memory.sramSize - 1] = 1;
			_GBMBC6FlashDirty(gb);
		}
		break;
	case GB_MBC6_FLASH_OP_UNPROTECT:
		if (state->flashOperationWriteEnable) {
			state->flashSector0Protected = false;
			gb->memory.sram[gb->memory.sramSize - 1] = 0;
			_GBMBC6FlashDirty(gb);
		}
		break;
	}
	memset(state->flashProgramWritten, 0, sizeof(state->flashProgramWritten));
	state->flashOperationBusy = false;
}

static void _GBMBC6FlashWrite(struct GB* gb, uint16_t address, uint32_t offset, uint8_t value) {
	struct GBMBC6State* state = &gb->memory.mbcState.mbc6;
	if (!gb->memory.sram) {
		return;
	}
	if (state->flashOperationBusy) {
		return;
	}
	if (state->flashMode == 4 || state->flashMode == 5) {
		uint8_t window = address >= GB_BASE_CART_HALFBANK2;
		uint32_t arrayOffset = _GBMBC6FlashArrayOffset(&gb->memory, address);
		uint8_t slot = offset & 0x7F;
		if (!state->flashProgramCount) {
			memset(state->flashProgramBuffer, 0xFF, sizeof(state->flashProgramBuffer));
			memset(state->flashProgramWritten, 0, sizeof(state->flashProgramWritten));
		}
		if (state->flashProgramLast == slot &&
		    (state->flashProgramWritten[slot >> 3] & (1 << (slot & 7)))) {
			if (value == 0xF0) {
				/* Reset on a repeated buffer slot exits without starting programming. */
				state->flashMode = 0;
				state->flashCommand = GB_MBC6_FLASH_CMD_IDLE;
				state->flashProgramCount = 0;
				memset(state->flashProgramWritten, 0, sizeof(state->flashProgramWritten));
				return;
			}
			/* Rewriting a buffer slot triggers programming. The trigger address
			 * selects the destination page; buffer-fill addresses select slots only. */
			uint32_t page = state->flashMode == 5
				? GB_SIZE_MBC6_FLASH + (offset & 0x80)
				: arrayOffset & ~0x7FU;
			uint32_t limit = GB_SIZE_MBC6_FLASH + (state->flashMode == 5 ? 0x100 : 0);
			if (page < limit && page + 0x80 <= limit) {
				_GBMBC6FlashStart(gb, GB_MBC6_FLASH_OP_PROGRAM, page, window, arrayOffset >> 13);
			} else {
				state->flashProgramCount = 0;
				state->flashMode = 2;
			}
			return;
		}
		if (!state->flashProgramCount) {
			state->flashProgramWindow = window;
			state->flashProgramBank = arrayOffset >> 13;
			state->flashProgramPage = arrayOffset & ~0x7FU;
		}
		if (!(state->flashProgramWritten[slot >> 3] & (1 << (slot & 7)))) {
			state->flashProgramWritten[slot >> 3] |= 1 << (slot & 7);
			++state->flashProgramCount;
		}
		state->flashProgramBuffer[slot] = value;
		state->flashProgramLast = slot;
		return;
	}
	if (value == 0xF0) {
		state->flashMode = 0;
		state->flashCommand = GB_MBC6_FLASH_CMD_IDLE;
		state->flashProgramCount = 0;
		return;
	}
	switch (state->flashCommand) {
	case GB_MBC6_FLASH_CMD_IDLE:
		if ((offset & 0x7FFF) == 0x5555 && value == 0xAA) {
			state->flashCommand = GB_MBC6_FLASH_CMD_UNLOCK_1;
		}
		break;
	case GB_MBC6_FLASH_CMD_UNLOCK_1:
		state->flashCommand = ((offset & 0x7FFF) == 0x2AAA && value == 0x55) ? GB_MBC6_FLASH_CMD_UNLOCK_2 : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_UNLOCK_2:
		state->flashCommand = GB_MBC6_FLASH_CMD_IDLE;
		if ((offset & 0x7FFF) != 0x5555) {
			break;
		}
		if (value == 0x90 || value == 0xA0 || value == 0x80 || value == 0x60 || value == 0x77) {
			/* Net de Get keeps the erase bank latched through F0, until a new opcode. */
			state->flashIoBankValid = false;
			state->flashOperationActive = false;
		}
		switch (value) {
		case 0x90:
			state->flashMode = 1;
			break;
		case 0xA0:
			state->flashMode = 4;
			state->flashProgramCount = 0;
			break;
		case 0x80:
			state->flashMode = 6;
			state->flashCommand = GB_MBC6_FLASH_CMD_ERASE_UNLOCK_1;
			break;
		case 0x60:
			state->flashCommand = GB_MBC6_FLASH_CMD_SPECIAL_UNLOCK_1;
			break;
		case 0x77:
			state->flashCommand = GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_1;
			break;
		}
		break;
	case GB_MBC6_FLASH_CMD_ERASE_UNLOCK_1:
		state->flashCommand = ((offset & 0x7FFF) == 0x5555 && value == 0xAA) ? GB_MBC6_FLASH_CMD_ERASE_UNLOCK_2 : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_ERASE_UNLOCK_2:
		state->flashCommand = ((offset & 0x7FFF) == 0x2AAA && value == 0x55) ? GB_MBC6_FLASH_CMD_ERASE_FINAL : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_ERASE_FINAL:
		state->flashCommand = GB_MBC6_FLASH_CMD_IDLE;
		if (value == 0x30) {
			uint8_t window = address >= GB_BASE_CART_HALFBANK2;
			uint32_t liveBank = window ? gb->memory.currentBank1 : gb->memory.currentBank;
			uint32_t liveOffset = (liveBank << 13) | (address & (GB_SIZE_CART_HALFBANK - 1));
			uint32_t sector = liveOffset & ~0x1FFFF;
			state->flashIoBankValid = false;
			_GBMBC6FlashStart(gb, GB_MBC6_FLASH_OP_ERASE_SECTOR, sector, window, liveBank);
		} else if (value == 0x10 && (offset & 0x7FFF) == 0x5555) {
			_GBMBC6FlashStart(gb, GB_MBC6_FLASH_OP_ERASE_CHIP, 0, address >= GB_BASE_CART_HALFBANK2, offset >> 13);
		}
		break;
	case GB_MBC6_FLASH_CMD_SPECIAL_UNLOCK_1:
		state->flashCommand = ((offset & 0x7FFF) == 0x5555 && value == 0xAA) ? GB_MBC6_FLASH_CMD_SPECIAL_UNLOCK_2 : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_SPECIAL_UNLOCK_2:
		state->flashCommand = ((offset & 0x7FFF) == 0x2AAA && value == 0x55) ? GB_MBC6_FLASH_CMD_SPECIAL_FINAL : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_SPECIAL_FINAL:
		state->flashCommand = GB_MBC6_FLASH_CMD_IDLE;
		if (value == 0xE0 && state->flashWriteEnable && (offset & 0x7FFF) == 0x5555) {
			state->flashMode = 5;
			state->flashProgramCount = 0;
		} else if (value == 0x04 && state->flashWriteEnable && (offset & 0x7FFF) == 0x5555) {
			_GBMBC6FlashStart(gb, GB_MBC6_FLASH_OP_ERASE_HIDDEN, GB_SIZE_MBC6_FLASH, address >= GB_BASE_CART_HALFBANK2, offset >> 13);
		} else if (value == 0x20 && state->flashWriteEnable && offset < 0x20000) {
			_GBMBC6FlashStart(gb, GB_MBC6_FLASH_OP_PROTECT, 0, address >= GB_BASE_CART_HALFBANK2, offset >> 13);
		} else if (value == 0x40 && state->flashWriteEnable && offset < 0x20000) {
			_GBMBC6FlashStart(gb, GB_MBC6_FLASH_OP_UNPROTECT, 0, address >= GB_BASE_CART_HALFBANK2, offset >> 13);
		}
		else if ((value == 0xE0 || value == 0x04 || value == 0x20 || value == 0x40) &&
		         (offset & 0x7FFF) == 0x5555 && !state->flashWriteEnable) {
			/* Protected commands are ignored by the chip and never enter status mode. */
			state->flashMode = 0;
		}
		break;
	case GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_1:
		state->flashCommand = ((offset & 0x7FFF) == 0x5555 && value == 0xAA) ? GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_2 : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_2:
		state->flashCommand = ((offset & 0x7FFF) == 0x2AAA && value == 0x55) ? GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_3 : GB_MBC6_FLASH_CMD_IDLE;
		break;
	case GB_MBC6_FLASH_CMD_HIDDEN_UNLOCK_3:
		state->flashCommand = GB_MBC6_FLASH_CMD_IDLE;
		if ((offset & 0x7FFF) == 0x5555 && value == 0x77) {
			state->flashMode = 3;
		}
		break;
	}
}

static void _GBMBC6MapChip(struct GB* gb, int half, uint8_t value) {
	if (!half) {
		gb->memory.mbcState.mbc6.flashBank0 = !!(value & 0x08);
		GBMBCSwitchHalfBank(gb, half, gb->memory.currentBank);
	} else {
		gb->memory.mbcState.mbc6.flashBank1 = !!(value & 0x08);
		GBMBCSwitchHalfBank(gb, half, gb->memory.currentBank1);
	}
}

void _GBMBC7(struct GB* gb, uint16_t address, uint8_t value) {
	int bank = value & 0x7F;
	switch (address >> 13) {
	case 0x0:
		switch (value) {
		default:
		case 0:
			gb->memory.mbcState.mbc7.access = 0;
			break;
		case 0xA:
			gb->memory.mbcState.mbc7.access |= 1;
			break;
		}
		break;
	case 0x1:
		GBMBCSwitchBank(gb, bank);
		break;
	case 0x2:
		if (value == 0x40) {
			gb->memory.mbcState.mbc7.access |= 2;
		} else {
			gb->memory.mbcState.mbc7.access &= ~2;
		}
		break;
	case 0x5:
		_GBMBC7Write(&gb->memory, address, value);
		gb->sramDirty |= mSAVEDATA_DIRT_NEW;
		break;
	default:
		// TODO
		mLOG(GB_MBC, STUB, "MBC7 unknown address: %04X:%02X", address, value);
		break;
	}
}

uint8_t _GBMBC7Read(struct GBMemory* memory, uint16_t address) {
	struct GBMBC7State* mbc7 = &memory->mbcState.mbc7;
	if (mbc7->access != 3) {
		return 0xFF;
	}
	switch (address & 0xF0) {
	case 0x20:
		if (memory->rotation && memory->rotation->readTiltX) {
			int32_t x = -memory->rotation->readTiltX(memory->rotation);
			x >>= 21;
			x += 0x81D0;
			return x;
		}
		return 0xFF;
	case 0x30:
		if (memory->rotation && memory->rotation->readTiltX) {
			int32_t x = -memory->rotation->readTiltX(memory->rotation);
			x >>= 21;
			x += 0x81D0;
			return x >> 8;
		}
		return 7;
	case 0x40:
		if (memory->rotation && memory->rotation->readTiltY) {
			int32_t y = -memory->rotation->readTiltY(memory->rotation);
			y >>= 21;
			y += 0x81D0;
			return y;
		}
		return 0xFF;
	case 0x50:
		if (memory->rotation && memory->rotation->readTiltY) {
			int32_t y = -memory->rotation->readTiltY(memory->rotation);
			y >>= 21;
			y += 0x81D0;
			return y >> 8;
		}
		return 7;
	case 0x60:
		return 0;
	case 0x80:
		return mbc7->eeprom;
	default:
		return 0xFF;
	}
}

void _GBMBC7Write(struct GBMemory* memory, uint16_t address, uint8_t value) {
	struct GBMBC7State* mbc7 = &memory->mbcState.mbc7;
	if (mbc7->access != 3) {
		return;
	}
	switch (address & 0xF0) {
	case 0x00:
		mbc7->latch = (value & 0x55) == 0x55;
		return;
	case 0x10:
		mbc7->latch |= (value & 0xAA);
		if (mbc7->latch == 0xAB && memory->rotation && memory->rotation->sample) {
			memory->rotation->sample(memory->rotation);
		}
		mbc7->latch = 0;
		return;
	default:
		mLOG(GB_MBC, STUB, "MBC7 unknown register: %04X:%02X", address, value);
		return;
	case 0x80:
		break;
	}
	GBMBC7Field old = memory->mbcState.mbc7.eeprom;
	value = GBMBC7FieldFillDO(value); // Hi-Z
	if (!GBMBC7FieldIsCS(old) && GBMBC7FieldIsCS(value)) {
		mbc7->state = GBMBC7_STATE_IDLE;
	}
	if (!GBMBC7FieldIsCLK(old) && GBMBC7FieldIsCLK(value)) {
		if (mbc7->state == GBMBC7_STATE_READ_COMMAND || mbc7->state == GBMBC7_STATE_EEPROM_WRITE || mbc7->state == GBMBC7_STATE_EEPROM_WRAL) {
			mbc7->sr <<= 1;
			mbc7->sr |= GBMBC7FieldGetDI(value);
			++mbc7->srBits;
		}
		switch (mbc7->state) {
		case GBMBC7_STATE_IDLE:
			if (GBMBC7FieldIsDI(value)) {
				mbc7->state = GBMBC7_STATE_READ_COMMAND;
				mbc7->srBits = 0;
				mbc7->sr = 0;
			}
			break;
		case GBMBC7_STATE_READ_COMMAND:
			if (mbc7->srBits == 10) {
				mbc7->state = 0x10 | (mbc7->sr >> 6);
				if (mbc7->state & 0xC) {
					mbc7->state &= ~0x3;
				}
				mbc7->srBits = 0;
				mbc7->address = mbc7->sr & 0x7F;
			}
			break;
		case GBMBC7_STATE_DO:
			value = GBMBC7FieldSetDO(value, mbc7->sr >> 15);
			mbc7->sr <<= 1;
			--mbc7->srBits;
			if (!mbc7->srBits) {
				mbc7->state = GBMBC7_STATE_IDLE;
			}
			break;
		default:
			break;
		}
		switch (mbc7->state) {
		case GBMBC7_STATE_EEPROM_EWEN:
			mbc7->writable = true;
			mbc7->state = GBMBC7_STATE_IDLE;
			break;
		case GBMBC7_STATE_EEPROM_EWDS:
			mbc7->writable = false;
			mbc7->state = GBMBC7_STATE_IDLE;
			break;
		case GBMBC7_STATE_EEPROM_WRITE:
			if (mbc7->srBits == 16) {
				if (mbc7->writable) {
					memory->sram[mbc7->address * 2] = mbc7->sr >> 8;
					memory->sram[mbc7->address * 2 + 1] = mbc7->sr;
				}
				mbc7->state = GBMBC7_STATE_IDLE;
			}
			break;
		case GBMBC7_STATE_EEPROM_ERASE:
			if (mbc7->writable) {
				memory->sram[mbc7->address * 2] = 0xFF;
				memory->sram[mbc7->address * 2 + 1] = 0xFF;
			}
			mbc7->state = GBMBC7_STATE_IDLE;
			break;
		case GBMBC7_STATE_EEPROM_READ:
			mbc7->srBits = 16;
			mbc7->sr = memory->sram[mbc7->address * 2] << 8;
			mbc7->sr |= memory->sram[mbc7->address * 2 + 1];
			mbc7->state = GBMBC7_STATE_DO;
			value = GBMBC7FieldClearDO(value);
			break;
		case GBMBC7_STATE_EEPROM_WRAL:
			if (mbc7->srBits == 16) {
				if (mbc7->writable) {
					int i;
					for (i = 0; i < 128; ++i) {
						memory->sram[i * 2] = mbc7->sr >> 8;
						memory->sram[i * 2 + 1] = mbc7->sr;
					}
				}
				mbc7->state = GBMBC7_STATE_IDLE;
			}
			break;
		case GBMBC7_STATE_EEPROM_ERAL:
			if (mbc7->writable) {
				int i;
				for (i = 0; i < 128; ++i) {
					memory->sram[i * 2] = 0xFF;
					memory->sram[i * 2 + 1] = 0xFF;
				}
			}
			mbc7->state = GBMBC7_STATE_IDLE;
			break;
		default:
			break;
		}
	} else if (GBMBC7FieldIsCS(value) && GBMBC7FieldIsCLK(old) && !GBMBC7FieldIsCLK(value)) {
		value = GBMBC7FieldSetDO(value, GBMBC7FieldGetDO(old));
	}
	mbc7->eeprom = value;
}

void _GBM161(struct GB* gb, uint16_t address, uint8_t value) {
	UNUSED(address);

	struct GBM161State* m161 = &gb->memory.mbcState.m161;
	if (m161->locked) {
		return;
	}

	int bank = value & 0x7;
	m161->bank = bank;
	m161->locked = true;

	GBMBCSwitchBank0(gb, bank * 2);
	GBMBCSwitchBank(gb, bank * 2 + 1);
}
