// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <wx/artprov.h>
#include <wx/string.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/menu.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/frame.h>
#include <wx/filedlg.h>

//Lua include stuff
#ifdef __cplusplus
#include <lua.hpp>
#else
#include <lua.h>
#include <lualib.h>
#include <laux.h>
#endif

class LuaScriptFrame;

class LuaThread : public wxThread
{
public:
  LuaThread(LuaScriptFrame* p, wxString file);
  ~LuaThread();

  wxThread::ExitCode Entry();

private:
  LuaScriptFrame* parent;
  wxString file_path;
};


int printToTextCtrl(lua_State* L);

class LuaScriptFrame final : public wxFrame
{
private:
  void CreateGUI();
  void OnClearClicked(wxCommandEvent& event);
  void BrowseOnButtonClick(wxCommandEvent& event);
  void RunOnButtonClick(wxCommandEvent& event);
  void StopOnButtonClick(wxCommandEvent& event);
  wxMenuBar* m_menubar;
  wxMenu* m_menu;
  wxStaticText* script_file_label;
  wxTextCtrl* file_path;
  wxButton* Browse;
  wxButton* run_button;
  wxButton* stop_button;
  wxStaticText* m_staticText2;
  wxTextCtrl* output_console;
  LuaThread* lua_thread;

public:
  void Log(const char* message);
  void SignalThreadFinished();

  LuaScriptFrame(wxWindow* parent);

  ~LuaScriptFrame();

  void LuaScriptFrameOnContextMenu(wxMouseEvent &event)
  {
    this->PopupMenu(m_menu, event.GetPosition());
  }
};
