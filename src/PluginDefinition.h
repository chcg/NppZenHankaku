#pragma once

#include "PluginInterface.h"

const wchar_t NPP_PLUGIN_NAME[] = L"全角半角変換";

// すべて×5 + 区切り + すべて×5
const int nbFunc = 11;

void pluginInit(HANDLE hModule);
void pluginCleanUp();
void commandMenuInit();
void commandMenuCleanUp();
void addToolbarButtons();
bool setCommand(size_t index, const wchar_t* cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey* sk = nullptr, bool check0nInit = false);

void convertToHankakuAll();
void convertToHankakuAlphaNum();
void convertToHankakuKatakana();
void convertToHankakuSymbol();
void convertToHankakuSpace();

void convertToZenkakuAll();
void convertToZenkakuAlphaNum();
void convertToZenkakuKatakana();
void convertToZenkakuSymbol();
void convertToZenkakuSpace();
