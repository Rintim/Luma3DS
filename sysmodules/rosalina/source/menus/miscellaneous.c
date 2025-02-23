/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2021 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#include <3ds.h>
#include "menus/miscellaneous.h"
#include "input_redirection.h"
#include "ntp.h"
#include "memory.h"
#include "draw.h"
#include "hbloader.h"
#include "fmt.h"
#include "utils.h" // for makeArmBranch
#include "minisoc.h"
#include "ifile.h"
#include "pmdbgext.h"
#include "plugin.h"
#include "process_patches.h"

typedef struct DspFirmSegmentHeader {
    u32 offset;
    u32 loadAddrHalfwords;
    u32 size;
    u8 _0x0C[3];
    u8 memType;
    u8 hash[0x20];
} DspFirmSegmentHeader;

typedef struct DspFirm {
    u8 signature[0x100];
    char magic[4];
    u32 totalSize; // no more than 0x10000
    u16 layoutBitfield;
    u8 _0x10A[3];
    u8 surroundSegmentMemType;
    u8 numSegments; // no more than 10
    u8 flags;
    u32 surroundSegmentLoadAddrHalfwords;
    u32 surroundSegmentSize;
    u8 _0x118[8];
    DspFirmSegmentHeader segmentHdrs[10];
    u8 data[];
} DspFirm;

Menu miscellaneousMenu = {
    "其他设置",
    {
        { "将当前应用替换为homebrew", METHOD, .method = &MiscellaneousMenu_SwitchBoot3dsxTargetTitle },
        { "更改菜单呼出热键", METHOD, .method = &MiscellaneousMenu_ChangeMenuCombo },
        { "开始输入重定向", METHOD, .method = &MiscellaneousMenu_InputRedirection },
        { "通过NTP服务同步时间和日期", METHOD, .method = &MiscellaneousMenu_UpdateTimeDateNtp },
        { "取消用户时间偏移", METHOD, .method = &MiscellaneousMenu_NullifyUserTimeOffset },
        { "转储DSP固件", METHOD, .method = &MiscellaneousMenu_DumpDspFirm },
        { "保存设置", METHOD, .method = &MiscellaneousMenu_SaveSettings },
        {},
    }
};

void MiscellaneousMenu_SwitchBoot3dsxTargetTitle(void)
{
    Result res;
    char failureReason[64];

    if(Luma_SharedConfig->hbldr_3dsx_tid == HBLDR_DEFAULT_3DSX_TID)
    {
        FS_ProgramInfo progInfo;
        u32 pid;
        u32 launchFlags;
        res = PMDBG_GetCurrentAppInfo(&progInfo, &pid, &launchFlags);
        if(R_SUCCEEDED(res))
        {
            Luma_SharedConfig->hbldr_3dsx_tid = progInfo.programId;
            miscellaneousMenu.items[0].title = "将hblauncher_loader应用替换为homebrew";
        }
        else
        {
            res = -1;
            strcpy(failureReason, "没有找到可用的线程。");
        }
    }
    else
    {
        res = 0;
        Luma_SharedConfig->hbldr_3dsx_tid = HBLDR_DEFAULT_3DSX_TID;
        miscellaneousMenu.items[0].title = "将当前应用替换为homebrew";
    }

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");

        if(R_SUCCEEDED(res))
            Draw_DrawString(10, 30, COLOR_WHITE, "执行成功。");
        else
            Draw_DrawFormattedString(10, 30, COLOR_WHITE, "执行失败 (%s)。", failureReason);

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

static void MiscellaneousMenu_ConvertComboToString(char *out, u32 combo)
{
    static const char *keys[] = {
        "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y",
        "?", "?",
        "ZL", "ZR",
        "?", "?", "?", "?",
        "Touch",
        "?", "?", "?",
        "CStick Right", "CStick Left", "CStick Up", "CStick Down",
        "CPad Right", "CPad Left", "CPad Up", "CPad Down",
    };

    char *outOrig = out;
    out[0] = 0;
    for(s32 i = 31; i >= 0; i--)
    {
        if(combo & (1 << i))
        {
            strcpy(out, keys[i]);
            out += strlen(keys[i]);
            *out++ = '+';
        }
    }

    if (out != outOrig)
        out[-1] = 0;
}

void MiscellaneousMenu_ChangeMenuCombo(void)
{
    char comboStrOrig[128], comboStr[128];
    u32 posY;

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    MiscellaneousMenu_ConvertComboToString(comboStrOrig, menuCombo);

    Draw_Lock();
    Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");

    posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "当前的菜单热键是：  %s", comboStrOrig);
    posY = Draw_DrawString(10, posY + SPACING_Y + 4, COLOR_WHITE, "请键入新的按键：");

    posY = Draw_DrawString(10, 130, COLOR_RED, "提示：同时长按后松开可设定组合键。");

    menuCombo = waitCombo();
    MiscellaneousMenu_ConvertComboToString(comboStr, menuCombo);

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");

        posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "当前的菜单热键是：  %s", comboStrOrig);
        posY = Draw_DrawFormattedString(10, posY + SPACING_Y + 4, COLOR_WHITE, "请键入新的按键：%s", comboStr) + SPACING_Y;

        posY = Draw_DrawString(10, posY + SPACING_Y, COLOR_WHITE, "菜单热键已设置成功！");

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

Result  SaveSettings(void)
{
    Result res;

    IFile file;
    u64 total;

    struct PACKED ALIGN(4)
    {
        char magic[4];
        u16 formatVersionMajor, formatVersionMinor;

        u32 config, multiConfig, bootConfig;
        u64 hbldr3dsxTitleId;
        u32 rosalinaMenuCombo;
        u32 rosalinaFlags;
    } configData;

    u32 formatVersion;
    u32 config, multiConfig, bootConfig;
    s64 out;
    bool isSdMode;

    if(R_FAILED(svcGetSystemInfo(&out, 0x10000, 2))) svcBreak(USERBREAK_ASSERT);
    formatVersion = (u32)out;
    if(R_FAILED(svcGetSystemInfo(&out, 0x10000, 3))) svcBreak(USERBREAK_ASSERT);
    config = (u32)out;
    if(R_FAILED(svcGetSystemInfo(&out, 0x10000, 4))) svcBreak(USERBREAK_ASSERT);
    multiConfig = (u32)out;
    if(R_FAILED(svcGetSystemInfo(&out, 0x10000, 5))) svcBreak(USERBREAK_ASSERT);
    bootConfig = (u32)out;
    if(R_FAILED(svcGetSystemInfo(&out, 0x10000, 0x203))) svcBreak(USERBREAK_ASSERT);
    isSdMode = (bool)out;

    memcpy(configData.magic, "CONF", 4);
    configData.formatVersionMajor = (u16)(formatVersion >> 16);
    configData.formatVersionMinor = (u16)formatVersion;
    configData.config = config;
    configData.multiConfig = multiConfig;
    configData.bootConfig = bootConfig;
    configData.hbldr3dsxTitleId = Luma_SharedConfig->hbldr_3dsx_tid;
    configData.rosalinaMenuCombo = menuCombo;
    configData.rosalinaFlags = PluginLoader__IsEnabled();

    FS_ArchiveID archiveId = isSdMode ? ARCHIVE_SDMC : ARCHIVE_NAND_RW;
    res = IFile_Open(&file, archiveId, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/luma/config.bin"), FS_OPEN_CREATE | FS_OPEN_WRITE);

    if(R_SUCCEEDED(res))
        res = IFile_SetSize(&file, sizeof(configData));
    if(R_SUCCEEDED(res))
        res = IFile_Write(&file, &total, &configData, sizeof(configData), 0);
    IFile_Close(&file);

    IFile_Close(&file);
    return res;
}

void MiscellaneousMenu_SaveSettings(void)
{
    Result res = SaveSettings();

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");
        if(R_SUCCEEDED(res))
            Draw_DrawString(10, 30, COLOR_WHITE, "执行成功。");
        else
            Draw_DrawFormattedString(10, 30, COLOR_WHITE, "执行失败 (0x%08lx)。", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

void MiscellaneousMenu_InputRedirection(void)
{
    bool done = false;

    Result res;
    char buf[65];
    bool wasEnabled = inputRedirectionEnabled;
    bool cantStart = false;

    if(wasEnabled)
    {
        res = InputRedirection_Disable(5 * 1000 * 1000 * 1000LL);
        if(res != 0)
            sprintf(buf, "停止输入重定向错误 (0x%08lx)。", (u32)res);
        else
            miscellaneousMenu.items[2].title = "开始输入重定向";
    }
    else
    {
        s64     dummyInfo;
        bool    isN3DS = svcGetSystemInfo(&dummyInfo, 0x10001, 0) == 0;
        bool    isSocURegistered;

        res = srvIsServiceRegistered(&isSocURegistered, "soc:U");
        cantStart = R_FAILED(res) || !isSocURegistered;

        if(!cantStart && isN3DS)
        {
            bool    isIrRstRegistered;

            res = srvIsServiceRegistered(&isIrRstRegistered, "ir:rst");
            cantStart = R_FAILED(res) || !isIrRstRegistered;
        }
    }

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");

        if(!wasEnabled && cantStart)
            Draw_DrawString(10, 30, COLOR_WHITE, "不能开始输入重定向，请在系统加载完成后再试。");
        else if(!wasEnabled)
        {
            Draw_DrawString(10, 30, COLOR_WHITE, "开始输入重定向...");
            if(!done)
            {
                res = InputRedirection_DoOrUndoPatches();
                if(R_SUCCEEDED(res))
                {
                    res = svcCreateEvent(&inputRedirectionThreadStartedEvent, RESET_STICKY);
                    if(R_SUCCEEDED(res))
                    {
                        inputRedirectionCreateThread();
                        res = svcWaitSynchronization(inputRedirectionThreadStartedEvent, 10 * 1000 * 1000 * 1000LL);
                        if(res == 0)
                            res = (Result)inputRedirectionStartResult;

                        if(res != 0)
                        {
                            svcCloseHandle(inputRedirectionThreadStartedEvent);
                            InputRedirection_DoOrUndoPatches();
                            inputRedirectionEnabled = false;
                        }
                        inputRedirectionStartResult = 0;
                    }
                }

                if(res != 0)
                    sprintf(buf, "开始输入重定向... 失败 (0x%08lx)。", (u32)res);
                else
                    miscellaneousMenu.items[2].title = "停止输入重定向";

                done = true;
            }

            if(res == 0)
                Draw_DrawString(10, 30, COLOR_WHITE, "开始输入重定向... 完成。");
            else
                Draw_DrawString(10, 30, COLOR_WHITE, buf);
        }
        else
        {
            if(res == 0)
            {
                u32 posY = 30;
                posY = Draw_DrawString(10, posY, COLOR_WHITE, "停止输入重定向成功。\n\n");
                if (isN3DS)
                {
                    posY = Draw_DrawString(
                        10,
                        posY,
                        COLOR_WHITE,
                        "这可能会无缘无故在主菜单上发生重复按键，\n这时只需要按一下ZL/ZR就可以了。"
                    );
                }
            }
            else
                Draw_DrawString(10, 30, COLOR_WHITE, buf);
        }

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

void MiscellaneousMenu_UpdateTimeDateNtp(void)
{
    u32 posY;
    u32 input = 0;

    Result res;
    bool cantStart = false;

    bool isSocURegistered;

    u64 msSince1900, samplingTick;

    res = srvIsServiceRegistered(&isSocURegistered, "soc:U");
    cantStart = R_FAILED(res) || !isSocURegistered;

    int utcOffset = 12;
	int utcOffsetMinute = 0;
    int absOffset;
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");

        absOffset = utcOffset - 12;
        absOffset = absOffset < 0 ? -absOffset : absOffset;
        posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "当前UTC偏移： %c%02d%02d", utcOffset < 12 ? '-' : '+', absOffset, utcOffsetMinute);
        posY = Draw_DrawFormattedString(10, posY + SPACING_Y + 4, COLOR_WHITE, "使用方向键 左/右 更改小时。");
        posY = Draw_DrawFormattedString(10, posY + SPACING_Y + 4, COLOR_WHITE, "使用方向键 上/下 更改分钟。");
        posY = Draw_DrawFormattedString(10, posY + SPACING_Y + 4, COLOR_WHITE, "然后按A完成。") + SPACING_Y;

        input = waitInput();

        if(input & KEY_LEFT) utcOffset = (24 + utcOffset - 1) % 24; // ensure utcOffset >= 0
        if(input & KEY_RIGHT) utcOffset = (utcOffset + 1) % 24;
        if(input & KEY_UP) utcOffsetMinute = (utcOffsetMinute + 1) % 60;
        if(input & KEY_DOWN) utcOffsetMinute = (60 + utcOffsetMinute - 1) % 60;
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(input & (KEY_A | KEY_B)) && !menuShouldExit);

    if (input & KEY_B)
        return;

    utcOffset -= 12;

    res = srvIsServiceRegistered(&isSocURegistered, "soc:U");
    cantStart = R_FAILED(res) || !isSocURegistered;
    res = 0;
    if(!cantStart)
    {
        res = ntpGetTimeStamp(&msSince1900, &samplingTick);
        if(R_SUCCEEDED(res))
        {
            msSince1900 += 1000 * (3600 * utcOffset + 60 * utcOffsetMinute);
            res = ntpSetTimeDate(msSince1900, samplingTick);
        }
    }

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");

        absOffset = utcOffset;
        absOffset = absOffset < 0 ? -absOffset : absOffset;
        Draw_DrawFormattedString(10, 30, COLOR_WHITE, "当前UTC偏移： %c%02d", utcOffset < 0 ? '-' : '+', absOffset);
        if (cantStart)
            Draw_DrawFormattedString(10, posY + 2 * SPACING_Y, COLOR_WHITE, "在系统结束加载前不能同步时间/日期。") + SPACING_Y;
        else if (R_FAILED(res))
            Draw_DrawFormattedString(10, posY + 2 * SPACING_Y, COLOR_WHITE, "执行失败 (%08lx)。", (u32)res) + SPACING_Y;
        else
            Draw_DrawFormattedString(10, posY + 2 * SPACING_Y, COLOR_WHITE, "时间日期更新成功。") + SPACING_Y;

        input = waitInput();

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(input & KEY_B) && !menuShouldExit);

}

void MiscellaneousMenu_NullifyUserTimeOffset(void)
{
    Result res = ntpNullifyUserTimeOffset();

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");
        if(R_SUCCEEDED(res))
            Draw_DrawString(10, 30, COLOR_WHITE, "执行成功!\n\n请重启以应用更改。");
        else
            Draw_DrawFormattedString(10, 30, COLOR_WHITE, "执行失败 (%08lx)。", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

static Result MiscellaneousMenu_DumpDspFirmCallback(Handle procHandle, u32 textSz, u32 roSz, u32 rwSz)
{
    (void)procHandle;
    Result res = 0;

    // NOTE: we suppose .text, .rodata, .data+.bss are contiguous & in that order
    u32 rwStart = 0x00100000 + textSz + roSz;
    u32 rwEnd = rwStart + rwSz;

    // Locate the DSP firm (it's in .data, not .rodata, suprisingly)
    u32 magic;
    memcpy(&magic, "DSP1", 4);
    const u32 *off = (u32 *)rwStart;

    for (; off < (u32 *)rwEnd && *off != magic; off++);

    if (off >= (u32 *)rwEnd || off < (u32 *)(rwStart + 0x100))
        return -2;

    // Do some sanity checks
    const DspFirm *firm = (const DspFirm *)((u32)off - 0x100);
    if (firm->totalSize > 0x10000 || firm->numSegments > 10)
        return -3;
    if ((u32)firm + firm->totalSize >= rwEnd)
        return -3;

    // Dump to SD card (no point in dumping to CTRNAND as 3dsx stuff doesn't work there)
    IFile file;
    res = IFile_Open(
        &file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/dspfirm.cdc"), FS_OPEN_CREATE | FS_OPEN_WRITE
    );

    u64 total;
    if(R_SUCCEEDED(res))
        res = IFile_Write(&file, &total, firm, firm->totalSize, 0);
    if(R_SUCCEEDED(res))
        res = IFile_SetSize(&file, firm->totalSize); // truncate accordingly

    IFile_Close(&file);

    return res;
}
void MiscellaneousMenu_DumpDspFirm(void)
{
    Result res = OperateOnProcessByName("menu", MiscellaneousMenu_DumpDspFirmCallback);

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "其他设置");
        if(R_SUCCEEDED(res))
            Draw_DrawString(10, 30, COLOR_WHITE, "DSP固件已经成功写入到SD卡中的\n/3ds/dspfirm.cdc文件。");
        else
            Draw_DrawFormattedString(
                10, 30, COLOR_WHITE,
                "执行失败 (%08lx)。\n\n请保证主页正在运行且SD卡已插入。",
                res
            );
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}
