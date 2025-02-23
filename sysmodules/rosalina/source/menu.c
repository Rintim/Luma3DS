/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2022 Aurora Wright, TuxSH
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
#include "menu.h"
#include "draw.h"
#include "fmt.h"
#include "memory.h"
#include "ifile.h"
#include "menus.h"
#include "utils.h"
#include "menus/n3ds.h"
#include "menus/cheats.h"
#include "minisoc.h"
#include "plugin.h"

u32 menuCombo = 0;
bool isHidInitialized = false;
u32 mcuFwVersion = 0;

// libctru redefinition:

bool hidShouldUseIrrst(void)
{
    // ir:rst exposes only two sessions :(
    return false;
}

static inline u32 convertHidKeys(u32 keys)
{
    // Nothing to do yet
    return keys;
}

u32 waitInputWithTimeout(s32 msec)
{
    s32 n = 0;
    u32 keys;

    do
    {
        svcSleepThread(1 * 1000 * 1000LL);
        Draw_Lock();
        if (!isHidInitialized || menuShouldExit)
        {
            keys = 0;
            Draw_Unlock();
            break;
        }
        n++;

        hidScanInput();
        keys = convertHidKeys(hidKeysDown()) | (convertHidKeys(hidKeysDownRepeat()) & DIRECTIONAL_KEYS);
        Draw_Unlock();
    } while (keys == 0 && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec));


    return keys;
}

u32 waitInput(void)
{
    return waitInputWithTimeout(-1);
}

static u32 scanHeldKeys(void)
{
    u32 keys;

    Draw_Lock();

    if (!isHidInitialized || menuShouldExit)
        keys = 0;
    else
    {
        hidScanInput();
        keys = convertHidKeys(hidKeysHeld());
    }

    Draw_Unlock();
    return keys;
}

u32 waitComboWithTimeout(s32 msec)
{
    s32 n = 0;
    u32 keys = 0;
    u32 tempKeys = 0;

    // Wait for nothing to be pressed
    while (scanHeldKeys() != 0 && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec))
    {
        svcSleepThread(1 * 1000 * 1000LL);
        n++;
    }

    if (menuShouldExit || !isHidInitialized || !(msec < 0 || n < msec))
        return 0;

    do
    {
        svcSleepThread(1 * 1000 * 1000LL);
        n++;

        tempKeys = scanHeldKeys();

        for (u32 i = 0x10000; i > 0; i--)
        {
            if (tempKeys != scanHeldKeys()) break;
            if (i == 1) keys = tempKeys;
        }
    }
    while((keys == 0 || scanHeldKeys() != 0) && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec));

    return keys;
}

u32 waitCombo(void)
{
    return waitComboWithTimeout(-1);
}

static MyThread menuThread;
static u8 ALIGN(8) menuThreadStack[0x1000];

static u32 homeBtnPressed = 0;

static float batteryPercentage;
static float batteryVoltage;
static u8 batteryTemperature;

static Result menuUpdateMcuInfo(void)
{
    Result res = 0;
    u8 data[4];

    if (!isServiceUsable("mcu::HWC"))
        return -1;

    res = mcuHwcInit();
    if (R_FAILED(res))
        return res;

    // Read single-byte mcu regs 0x0A to 0x0D directly
    res = MCUHWC_ReadRegister(0xA, data, 4);

    if (R_SUCCEEDED(res))
    {
        batteryTemperature = data[0];

        // The battery percentage isn't very precise... its precision ranges from 0.09% to 0.14% approx
        // Round to 0.1%
        batteryPercentage = data[1] + data[2] / 256.0f;
        batteryPercentage = (u32)((batteryPercentage + 0.05f) * 10.0f) / 10.0f;

        // Round battery voltage to 0.01V
        batteryVoltage = (5u * data[3]) / 256.0f;
        batteryVoltage = (u32)((batteryVoltage + 0.005f) * 100.0f) / 100.0f;
    }

    // Read mcu fw version if not already done
    if (mcuFwVersion == 0)
    {
        u8 minor = 0, major = 0;
        MCUHWC_GetFwVerHigh(&major);
        MCUHWC_GetFwVerLow(&minor);

        // If it has failed, mcuFwVersion will be set to 0 again
        mcuFwVersion = SYSTEM_VERSION(major - 0x10, minor, 0);
    }

    mcuHwcExit();
    return res;
}

static inline u32 menuAdvanceCursor(u32 pos, u32 numItems, s32 displ)
{
    return (pos + numItems + displ) % numItems;
}

static inline bool menuItemIsHidden(const MenuItem *item)
{
    return item->visibility != NULL && !item->visibility();
}

bool menuCheckN3ds(void)
{
    return isN3DS;
}

u32 menuCountItems(const Menu *menu)
{
    u32 n;
    for (n = 0; menu->items[n].action_type != MENU_END; n++);
    return n;
}

u32 menuCountItemsWithoutHidden(const Menu *menu)
{
    u32 n;
    u32 m = 0;
    for (n = 0; menu->items[n].action_type != MENU_END; n++){
        if(menu->items[n].visibility == NULL || menu->items[n].visibility())
            m++;
    };
    return m;
}

MyThread *menuCreateThread(void)
{
    if(R_FAILED(MyThread_Create(&menuThread, menuThreadMain, menuThreadStack, 0x1000, 52, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &menuThread;
}

u32 menuCombo;
u32 g_blockMenuOpen = 0;

u32     DispWarningOnHome(void);

void menuThreadMain(void)
{
    if(isN3DS)
        N3DSMenu_UpdateStatus();

    while (!isServiceUsable("ac:u") || !isServiceUsable("hid:USER"))
        svcSleepThread(500 * 1000 * 1000LL);

    hidInit(); // assume this doesn't fail
    isHidInitialized = true;

    while(!preTerminationRequested)
    {
        svcSleepThread(50 * 1000 * 1000LL);
        if (menuShouldExit)
            continue;

        Cheat_ApplyCheats();

        if(((scanHeldKeys() & menuCombo) == menuCombo) && !g_blockMenuOpen)
        {
            menuEnter();
            if(isN3DS) N3DSMenu_UpdateStatus();
            PluginLoader__UpdateMenu();
            menuShow(&rosalinaMenu);
            menuLeave();
        }

        // Check for home button on O3DS Mode3 with plugin loaded
        if (homeBtnPressed != 0)
        {
            if (DispWarningOnHome())
                svcKernelSetState(7); ///< reboot is fine since exiting a mode3 game reboot anyway

            homeBtnPressed = 0;
        }
    }
}

static s32 menuRefCount = 0;
void menuEnter(void)
{
    Draw_Lock();
    if(!menuShouldExit && menuRefCount == 0)
    {
        menuRefCount++;
        svcKernelSetState(0x10000, 2 | 1);
        svcSleepThread(5 * 1000 * 100LL);
        if (R_FAILED(Draw_AllocateFramebufferCache(FB_BOTTOM_SIZE)))
        {
            // Oops
            menuRefCount = 0;
            svcKernelSetState(0x10000, 2 | 1);
            svcSleepThread(5 * 1000 * 100LL);
        }
        else
            Draw_SetupFramebuffer();
    }
    Draw_Unlock();
}

void menuLeave(void)
{
    svcSleepThread(50 * 1000 * 1000);

    Draw_Lock();
    if(--menuRefCount == 0)
    {
        Draw_RestoreFramebuffer();
        Draw_FreeFramebufferCache();
        svcKernelSetState(0x10000, 2 | 1);
    }
    Draw_Unlock();
}

static void menuDraw(Menu *menu, s32 selected,s32 page)
{
    char versionString[16];
    s64 out;
    u32 version, commitHash;
    bool isRelease;

    Result mcuInfoRes = menuUpdateMcuInfo();

    if (menuItemIsHidden(&menu->items[selected]))
        selected += 1;

    svcGetSystemInfo(&out, 0x10000, 0);
    version = (u32)out;

    svcGetSystemInfo(&out, 0x10000, 1);
    commitHash = (u32)out;

    svcGetSystemInfo(&out, 0x10000, 0x200);
    isRelease = (bool)out;

    if(GET_VERSION_REVISION(version) == 0)
        sprintf(versionString, "v%lu.%lu", GET_VERSION_MAJOR(version), GET_VERSION_MINOR(version));
    else
        sprintf(versionString, "v%lu.%lu.%lu", GET_VERSION_MAJOR(version), GET_VERSION_MINOR(version), GET_VERSION_REVISION(version));

    Draw_DrawString(16, 16, COLOR_TITLE, menu->title);
    s32 numItems = menuCountItems(menu);
    u32 dispY = 0;

    for(s32 i = 0; i < MAIN_PER_MENU_PAGE && page * MAIN_PER_MENU_PAGE + i < numItems; i++)
    {
        if (menuItemIsHidden(&menu->items[page * MAIN_PER_MENU_PAGE + i]))
            continue;
        Draw_DrawString(48, 44 + dispY, COLOR_WHITE, menu->items[page * MAIN_PER_MENU_PAGE + i].title);
        Draw_DrawCharacter(32, 44 + dispY, COLOR_TITLE, page * MAIN_PER_MENU_PAGE + i == selected ? '>' : ' ');
        dispY += SPACING_Y + 4;
    }

    if(miniSocEnabled)
    {
        char ipBuffer[17];
        u32 ip = socGethostid();
        u8 *addr = (u8 *)&ip;
        int n = sprintf(ipBuffer, "%hhu.%hhu.%hhu.%hhu", addr[0], addr[1], addr[2], addr[3]);
        Draw_DrawString(SCREEN_BOT_WIDTH - 16 - (SPACING_X/2) * n, 16, COLOR_WHITE, ipBuffer);
    }
    else
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 16 - (SPACING_X/2) * 15, 16, COLOR_WHITE, "%15s", "");

    if(R_SUCCEEDED(mcuInfoRes))
    {
        u32 voltageInt = (u32)batteryVoltage;
        u32 voltageFrac = (u32)(batteryVoltage * 100.0f) % 100u;
        u32 percentageInt = (u32)batteryPercentage;
        u32 percentageFrac = (u32)(batteryPercentage * 10.0f) % 10u;

//        char buf[32];
//        sprintf(
//            buf, "温度：%02hhu℃  电压：%lu.%02luV  剩余电量：%lu.%lu%%", batteryTemperature, // CP437
//            voltageInt, voltageFrac,
//            percentageInt, percentageFrac
//        );
        Draw_DrawFormattedString(16, SCREEN_BOT_HEIGHT - 16, COLOR_WHITE, "温度：%02hhu°C  电压：%lu.%02luV  电量：%lu.%lu%%",batteryTemperature, // CP437
                        voltageInt, voltageFrac,
                        percentageInt, percentageFrac);
    }
    else
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 10 - SPACING_X * 19, SCREEN_BOT_HEIGHT - 20, COLOR_WHITE, "%19s", "");

    if(isRelease)
        Draw_DrawFormattedString(16, SCREEN_BOT_HEIGHT - 32, COLOR_TITLE, "Luma3DS %s 中文版", versionString);
    else
        Draw_DrawFormattedString(16, SCREEN_BOT_HEIGHT - 32, COLOR_TITLE, "Luma3DS %s 中文版-%08lx", versionString, commitHash);

    Draw_FlushFramebuffer();
}

void menuShow(Menu *root)
{
    s32 selectedItem = 0, page = 0, pagePrev = 0;;
    Menu *currentMenu = root;
    u32 nbPreviousMenus = 0;
    Menu *previousMenus[0x80];
    u32 previousSelectedItems[0x80];

    s32 numItems = menuCountItems(currentMenu);
    if (menuItemIsHidden(&currentMenu->items[selectedItem]))
        selectedItem = menuAdvanceCursor(selectedItem, numItems, 1);
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    hidSetRepeatParameters(0, 0);
    menuDraw(currentMenu, selectedItem, page);
    Draw_Unlock();

    bool menuComboReleased = false;

    do
    {
        u32 pressed = waitInputWithTimeout(1000);

        if(!menuComboReleased && (scanHeldKeys() & menuCombo) != menuCombo)
        {
            menuComboReleased = true;
            Draw_Lock();
            hidSetRepeatParameters(200, 100);
            Draw_Unlock();
        }

        if(pressed & KEY_A)
        {
            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();

            switch(currentMenu->items[selectedItem].action_type)
            {
                case METHOD:
                    if(currentMenu->items[selectedItem].method != NULL)
                        currentMenu->items[selectedItem].method();
                    break;
                case MENU:
                    previousSelectedItems[nbPreviousMenus] = selectedItem;
                    previousMenus[nbPreviousMenus++] = currentMenu;
                    currentMenu = currentMenu->items[selectedItem].menu;
                    selectedItem = 0;
                    break;
                default:
                    __builtin_trap(); // oops
                    break;
            }

            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();
        }
        else if(pressed & KEY_B)
        {
            while (nbPreviousMenus == 0 && (scanHeldKeys() & KEY_B)); // wait a bit before exiting rosalina

            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();

            if(nbPreviousMenus > 0)
            {
                currentMenu = previousMenus[--nbPreviousMenus];
                selectedItem = previousSelectedItems[nbPreviousMenus];
            }
            else
                break;
        }
        else if(pressed & KEY_DOWN)
        {
            selectedItem = menuAdvanceCursor(selectedItem, numItems, 1);
            if (menuItemIsHidden(&currentMenu->items[selectedItem]))
                selectedItem = menuAdvanceCursor(selectedItem, numItems, 1);
        }
        else if(pressed & KEY_UP)
        {
            selectedItem = menuAdvanceCursor(selectedItem, numItems, -1);
            if (menuItemIsHidden(&currentMenu->items[selectedItem]))
                selectedItem = menuAdvanceCursor(selectedItem, numItems, -1);
        }
        else if(pressed & KEY_LEFT){
            if(selectedItem - MAIN_PER_MENU_PAGE > 0){
                for(int i = 0;i < MAIN_PER_MENU_PAGE; i++) {
                    selectedItem = menuAdvanceCursor(selectedItem, numItems, -1);
                    if (menuItemIsHidden(&currentMenu->items[selectedItem]))
                        selectedItem = menuAdvanceCursor(selectedItem, numItems, -1);
                }
            }
            else{
                s32 numItemsWithoutHidden = menuCountItemsWithoutHidden(currentMenu);
                s32 nullItem = MAIN_PER_MENU_PAGE - numItemsWithoutHidden % MAIN_PER_MENU_PAGE;
                if(selectedItem >= MAIN_PER_MENU_PAGE - nullItem){
                    selectedItem = 0;
                }else{
                    for(int i = 0;i < MAIN_PER_MENU_PAGE - nullItem; i++) {
                        selectedItem = menuAdvanceCursor(selectedItem, numItems, -1);
                        if (menuItemIsHidden(&currentMenu->items[selectedItem]))
                            selectedItem = menuAdvanceCursor(selectedItem, numItems, -1);
                    }
                }
            }
        }
        else if(pressed & KEY_RIGHT)
        {
            s32 numItemsWithoutHidden = menuCountItemsWithoutHidden(currentMenu);
            if(selectedItem + MAIN_PER_MENU_PAGE + 1 < numItemsWithoutHidden){
                for(int i = 0;i < MAIN_PER_MENU_PAGE; i++){
                    selectedItem = menuAdvanceCursor(selectedItem, numItems, 1);
                    if (menuItemIsHidden(&currentMenu->items[selectedItem]))
                        selectedItem = menuAdvanceCursor(selectedItem, numItems, 1);
                }
            }else if((selectedItem - 1) / MAIN_PER_MENU_PAGE == page){
                if(numItems - selectedItem == 1){
                    selectedItem = 0;
                }else{
                    selectedItem = numItems - 1;
                }
            }
            else
                selectedItem = numItems - 1;

        }

        numItems = menuCountItems(currentMenu);

        if(selectedItem < 0)
            selectedItem = numItems - 1;
        else if(selectedItem >= numItems)
            selectedItem = 0;
        pagePrev = page;
        page = selectedItem / MAIN_PER_MENU_PAGE;

        Draw_Lock();
        if(page != pagePrev)
            Draw_ClearFramebuffer();
        menuDraw(currentMenu, selectedItem, page);
        Draw_Unlock();
    }
    while(!menuShouldExit);
}
