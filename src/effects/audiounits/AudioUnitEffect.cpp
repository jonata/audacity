/**********************************************************************

  Audacity: A Digital Audio Editor

  AudioUnitEffect.cpp

  Dominic Mazzoni
  Leland Lucius

*******************************************************************//**

\class AudioUnitEffect
\brief An Effect class that handles a wide range of effects.  ??Mac only??

*//*******************************************************************/

#include "../../Audacity.h" // for USE_* macros

#if USE_AUDIO_UNITS
#include "AudioUnitEffect.h"

#include <wx/defs.h>
#include <wx/base64.h>
#include <wx/button.h>
#include <wx/control.h>
#include <wx/crt.h>
#include <wx/dir.h>
#include <wx/ffile.h>

#ifdef __WXMAC__
#include <wx/evtloop.h>
#endif

#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>

#include "../../ShuttleGui.h"
#include "../../widgets/AudacityMessageBox.h"
#include "../../widgets/valnum.h"
#include "../../widgets/wxPanelWrapper.h"

//
// When a plug-ins state is saved to the settings file (as a preset),
// it can be one of two formats, binary or XML.  In either case, it
// gets base64 encoded before storing.
//
// The advantages of XML format is less chance of failures occurring
// when exporting.  But, it can take a bit more space per preset int
// the Audacity settings file.
//
// Using binary for now.  Use kCFPropertyListXMLFormat_v1_0 if XML
// format is desired.
//
#define PRESET_FORMAT kCFPropertyListBinaryFormat_v1_0

// Name of the settings key to use for the above value
#define PRESET_KEY wxT("Data")

// Where the presets are located
#define PRESET_LOCAL_PATH wxT("/Library/Audio/Presets")
#define PRESET_USER_PATH wxT("~/Library/Audio/Presets")

struct CFReleaser
   { void operator () (const void *p) const { if (p) CFRelease(p); } };
template <typename T>
   using CFunique_ptr = std::unique_ptr<T, CFReleaser>;

class ParameterInfo
{
public:
   ParameterInfo()
   {
      info = {};
   }

   virtual ~ParameterInfo()
   {
      if (info.flags & kAudioUnitParameterFlag_HasCFNameString)
      {
         if (info.flags & kAudioUnitParameterFlag_CFNameRelease)
         {
            CFRelease(info.cfNameString);
         }
      }
   }

   bool Get(AudioUnit mUnit, AudioUnitParameterID parmId)
   {
      OSStatus result;
      UInt32 dataSize;

      info = {};
      dataSize = sizeof(info);
      result = AudioUnitGetProperty(mUnit,
                                    kAudioUnitProperty_ParameterInfo,
                                    kAudioUnitScope_Global,
                                    parmId,
                                    &info,
                                    &dataSize);  
      if (result != noErr)
      {
         return false;
      }

      if (info.flags & kAudioUnitParameterFlag_HasCFNameString)
      {
         name = wxCFStringRef::AsString(info.cfNameString);
      }
      else
      {
         name = wxString(info.name);
      }

      if (name.empty())
      {
         return false;
      }

      // If the parameter has a clumpID, then the final parameter name will be
      // either:
      //
      //    <clumpID,clumpName>ParameterName
      //
      // or (if the clumpName isn't available):
      //
      //    <clumpID>ParameterName
      if (info.flags & kAudioUnitParameterFlag_HasClump)
      {
         wxString clumpName;

         AudioUnitParameterNameInfo clumpInfo = {};
         clumpInfo.inID = info.clumpID;
         clumpInfo.inDesiredLength = kAudioUnitParameterName_Full;
         dataSize = sizeof(clumpInfo);

         result = AudioUnitGetProperty(mUnit,
                                       kAudioUnitProperty_ParameterClumpName,
                                       kAudioUnitScope_Global,
                                       0,
                                       &clumpInfo,
                                       &dataSize);  
         if (result == noErr)
         {
            clumpName.Printf(wxT("%c%s"),
                             idSep,
                             wxCFStringRef::AsString(clumpInfo.outName));
            clumpName.Replace(idEnd, wxT("_"));
         }

         name.Replace(idBeg, wxT('_'));
         name.Replace(idEnd, wxT('_'));
         name = wxString::Format(wxT("%c%x%s%c%s"),
                                 idBeg,
                                 info.clumpID,
                                 clumpName,
                                 idEnd,
                                 name);
      }

      return true;
   }

   static const char idBeg = wxT('<');
   static const char idSep = wxT(',');
   static const char idEnd = wxT('>');

   wxString name;
   AudioUnitParameterInfo info;
};

// ============================================================================
// Module registration entry point
//
// This is the symbol that Audacity looks for when the module is built as a
// dynamic library.
//
// When the module is builtin to Audacity, we use the same function, but it is
// declared static so as not to clash with other builtin modules.
// ============================================================================
DECLARE_MODULE_ENTRY(AudacityModule)
{
   // Create and register the importer
   // Trust the module manager not to leak this
   return safenew AudioUnitEffectsModule(moduleManager, path);
}

// ============================================================================
// Register this as a builtin module
// ============================================================================
DECLARE_BUILTIN_MODULE(AudioUnitEffectsBuiltin);

///////////////////////////////////////////////////////////////////////////////
//
// AudioUnitEffectsModule
//
///////////////////////////////////////////////////////////////////////////////

AudioUnitEffectsModule::AudioUnitEffectsModule(ModuleManagerInterface *moduleManager,
                                               const wxString *path)
{
   mModMan = moduleManager;
   if (path)
   {
      mPath = *path;
   }
}

AudioUnitEffectsModule::~AudioUnitEffectsModule()
{
   mPath.clear();
}

// ============================================================================
// ComponentInterface implementation
// ============================================================================

PluginPath AudioUnitEffectsModule::GetPath()
{
   return mPath;
}

ComponentInterfaceSymbol AudioUnitEffectsModule::GetSymbol()
{
   /* i18n-hint: Audio Unit is the name of an Apple audio software protocol */
   return XO("Audio Unit Effects");
}

VendorSymbol AudioUnitEffectsModule::GetVendor()
{
   return XO("The Audacity Team");
}

wxString AudioUnitEffectsModule::GetVersion()
{
   // This "may" be different if this were to be maintained as a separate DLL
   return AUDIOUNITEFFECTS_VERSION;
}

TranslatableString AudioUnitEffectsModule::GetDescription()
{
   return XO("Provides Audio Unit Effects support to Audacity");
}

// ============================================================================
// ModuleInterface implementation
// ============================================================================

const FileExtensions &AudioUnitEffectsModule::GetFileExtensions()
{
   static FileExtensions result{{ _T("au") }};
   return result;
}

bool AudioUnitEffectsModule::Initialize()
{
   // Nothing to do here
   return true;
}

void AudioUnitEffectsModule::Terminate()
{
   // Nothing to do here
   return;
}

EffectFamilySymbol AudioUnitEffectsModule::GetOptionalFamilySymbol()
{
#if USE_AUDIO_UNITS
   return AUDIOUNITEFFECTS_FAMILY;
#else
   return {};
#endif
}

bool AudioUnitEffectsModule::AutoRegisterPlugins(PluginManagerInterface & pm)
{
   // Nothing to be done here
   return true;
}

PluginPaths AudioUnitEffectsModule::FindPluginPaths(PluginManagerInterface & pm)
{
   PluginPaths effects;

   LoadAudioUnitsOfType(kAudioUnitType_Effect, effects);
   LoadAudioUnitsOfType(kAudioUnitType_Generator, effects);
   LoadAudioUnitsOfType(kAudioUnitType_MusicEffect, effects);
   LoadAudioUnitsOfType(kAudioUnitType_Mixer, effects);
   LoadAudioUnitsOfType(kAudioUnitType_Panner, effects);
   
   return effects;
}

unsigned AudioUnitEffectsModule::DiscoverPluginsAtPath(
   const PluginPath & path, TranslatableString &errMsg,
   const RegistrationCallback &callback)
{
   errMsg = {};
   wxString name;
   AudioComponent component = FindAudioUnit(path, name);
   if (component == NULL)
   {
      errMsg = XO("Could not find component");
      return 0;
   }

   AudioUnitEffect effect(path, name, component);
   if (!effect.SetHost(NULL))
   {
      // TODO:  Is it worth it to discriminate all the ways SetHost might
      // return false?
      errMsg = XO("Could not initialize component");
      return 0;
   }

   if(callback)
      callback(this, &effect);

   return 1;
}

bool AudioUnitEffectsModule::IsPluginValid(
   const PluginPath & path, bool bFast)
{
   if( bFast )
      return true;
   wxString name;
   return FindAudioUnit(path, name) != NULL;
}

ComponentInterface *AudioUnitEffectsModule::CreateInstance(const PluginPath & path)
{
   // Acquires a resource for the application.
   wxString name;
   AudioComponent component = FindAudioUnit(path, name);
   if (component == NULL)
   {
      return NULL;
   }

   // Safety of this depends on complementary calls to DeleteInstance on the module manager side.
   return safenew AudioUnitEffect(path, name, component);
}

void AudioUnitEffectsModule::DeleteInstance(ComponentInterface *instance)
{
   std::unique_ptr < AudioUnitEffect > {
      dynamic_cast<AudioUnitEffect *>(instance)
   };
}

// ============================================================================
// AudioUnitEffectsModule implementation
// ============================================================================

void AudioUnitEffectsModule::LoadAudioUnitsOfType(OSType inAUType,
                                                  PluginPaths & effects)
{
   AudioComponentDescription desc;
   AudioComponent component;

   desc.componentType = inAUType;
   desc.componentSubType = 0;
   desc.componentManufacturer = 0;
   desc.componentFlags = 0;
   desc.componentFlagsMask = 0;

   component = AudioComponentFindNext(NULL, &desc);
   while (component != NULL)
   {
      OSStatus result;
      AudioComponentDescription found;

      result = AudioComponentGetDescription(component, &found);
      if (result == noErr)
      {
         CFStringRef cfName{};
         result = AudioComponentCopyName(component, &cfName);
         CFunique_ptr<const __CFString> uName{ cfName };

         if (result == noErr)
         {
            wxString name = wxCFStringRef::AsString(cfName);
      
            effects.push_back(wxString::Format(wxT("%-4.4s/%-4.4s/%-4.4s/%s"),
                        FromOSType(found.componentManufacturer),
                        FromOSType(found.componentType),
                        FromOSType(found.componentSubType),
                        name));
         }
      }

      component = AudioComponentFindNext(component, &desc);
   }
}

AudioComponent AudioUnitEffectsModule::FindAudioUnit(const PluginPath & path,
                                                     wxString & name)
{
   wxStringTokenizer tokens(path, wxT("/"));

   AudioComponentDescription desc;

   desc.componentManufacturer = ToOSType(tokens.GetNextToken());
   desc.componentType = ToOSType(tokens.GetNextToken());
   desc.componentSubType = ToOSType(tokens.GetNextToken());
   desc.componentFlags = 0;
   desc.componentFlagsMask = 0;

   name = tokens.GetNextToken();

   return AudioComponentFindNext(NULL, &desc);
}

wxString AudioUnitEffectsModule::FromOSType(OSType type)
{
   OSType rev = (type & 0xff000000) >> 24 |
                (type & 0x00ff0000) >> 8  |
                (type & 0x0000ff00) << 8  |
                (type & 0x000000ff) << 24;
   
   return wxString::FromUTF8((char *)&rev, 4);
}

OSType AudioUnitEffectsModule::ToOSType(const wxString & type)
{
   wxCharBuffer buf = type.ToUTF8();

   OSType rev = ((unsigned char)buf.data()[0]) << 24 |
                ((unsigned char)buf.data()[1]) << 16 |
                ((unsigned char)buf.data()[2]) << 8 |
                ((unsigned char)buf.data()[3]);

   return rev;
}

///////////////////////////////////////////////////////////////////////////////
//
// AudioUnitEffectOptionsDialog
//
///////////////////////////////////////////////////////////////////////////////

class AudioUnitEffectOptionsDialog final : public wxDialogWrapper
{
public:
   AudioUnitEffectOptionsDialog(wxWindow * parent, EffectHostInterface *host);
   virtual ~AudioUnitEffectOptionsDialog();

   void PopulateOrExchange(ShuttleGui & S);

   void OnOk(wxCommandEvent & evt);

private:
   EffectHostInterface *mHost;

   bool mUseLatency;
   TranslatableString mUIType;

   DECLARE_EVENT_TABLE()
};

BEGIN_EVENT_TABLE(AudioUnitEffectOptionsDialog, wxDialogWrapper)
   EVT_BUTTON(wxID_OK, AudioUnitEffectOptionsDialog::OnOk)
END_EVENT_TABLE()

AudioUnitEffectOptionsDialog::AudioUnitEffectOptionsDialog(wxWindow * parent, EffectHostInterface *host)
:  wxDialogWrapper(parent, wxID_ANY, XO("Audio Unit Effect Options"))
{
   mHost = host;

   mHost->GetSharedConfig(wxT("Options"), wxT("UseLatency"), mUseLatency, true);

   // Expect one of three string values from the config file
   wxString uiType;
   mHost->GetSharedConfig(wxT("Options"), wxT("UIType"), uiType, wxT("Full"));

   // Get the localization of the string for display to the user
   mUIType = TranslatableString{ uiType, {} };

   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
}

AudioUnitEffectOptionsDialog::~AudioUnitEffectOptionsDialog()
{
}

void AudioUnitEffectOptionsDialog::PopulateOrExchange(ShuttleGui & S)
{
   
   S.SetBorder(5);
   S.StartHorizontalLay(wxEXPAND, 1);
   {
      S.StartVerticalLay(false);
      {
         S.StartStatic(XO("Latency Compensation"));
         {
            S.AddVariableText( XO(
"As part of their processing, some Audio Unit effects must delay returning "
"audio to Audacity. When not compensating for this delay, you will "
"notice that small silences have been inserted into the audio. "
"Enabling this option will provide that compensation, but it may "
"not work for all Audio Unit effects."),
               false, 0, 650 );

            S.StartHorizontalLay(wxALIGN_LEFT);
            {
               S.TieCheckBox(XO("Enable &compensation"),
                             mUseLatency);
            }
            S.EndHorizontalLay();
         }
         S.EndStatic();

         S.StartStatic(XO("User Interface"));
         {
            S.AddVariableText( XO(
"Select \"Full\" to use the graphical interface if supplied by the Audio Unit."
" Select \"Generic\" to use the system supplied generic interface."
" Select \"Basic\" for a basic text-only interface."
" Reopen the effect for this to take effect."),
               false, 0, 650);

            S.StartHorizontalLay(wxALIGN_LEFT);
            {
               S.TieChoice(XO("Select &interface"),
                  mUIType,
                  { XO("Full"), XO("Generic"), XO("Basic") });
            }
            S.EndHorizontalLay();
         }
         S.EndStatic();
      }
      S.EndVerticalLay();
   }
   S.EndHorizontalLay();

   S.AddStandardButtons();

   Layout();
   Fit();
   Center();
}

void AudioUnitEffectOptionsDialog::OnOk(wxCommandEvent & WXUNUSED(evt))
{
   if (!Validate())
   {
      return;
   }

   ShuttleGui S(this, eIsGettingFromDialog);
   PopulateOrExchange(S);

   // un-translate the type
   auto uiType = mUIType.MSGID().GET();

   mHost->SetSharedConfig(wxT("Options"), wxT("UseLatency"), mUseLatency);
   mHost->SetSharedConfig(wxT("Options"), wxT("UIType"), uiType);

   EndModal(wxID_OK);
}

///////////////////////////////////////////////////////////////////////////////
//
// AudioUnitEffectExportDialog
//
///////////////////////////////////////////////////////////////////////////////

class AudioUnitEffectExportDialog final : public wxDialogWrapper
{
public:
   AudioUnitEffectExportDialog(wxWindow * parent, AudioUnitEffect *effect);
   virtual ~AudioUnitEffectExportDialog();

   void PopulateOrExchange(ShuttleGui & S);
   wxString Export(const wxString & name);

   void OnOk(wxCommandEvent & evt);

private:
   wxWindow *mParent;
   AudioUnitEffect *mEffect;

   wxListCtrl *mList;

   DECLARE_EVENT_TABLE()
};

BEGIN_EVENT_TABLE(AudioUnitEffectExportDialog, wxDialogWrapper)
   EVT_BUTTON(wxID_OK, AudioUnitEffectExportDialog::OnOk)
END_EVENT_TABLE()

AudioUnitEffectExportDialog::AudioUnitEffectExportDialog(wxWindow * parent, AudioUnitEffect *effect)
:  wxDialogWrapper(parent, wxID_ANY, XO("Export Audio Unit Presets"))
{
   mEffect = effect;

   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
}

AudioUnitEffectExportDialog::~AudioUnitEffectExportDialog()
{
}

void AudioUnitEffectExportDialog::PopulateOrExchange(ShuttleGui & S)
{
   S.SetBorder(5);
   S.StartHorizontalLay(wxEXPAND, 1);
   {
      S.StartVerticalLay(true);
      {
         S.StartStatic(XO("Presets (may select multiple)"));
         {
            mList = S.Style(wxLC_REPORT | wxLC_HRULES | wxLC_VRULES |
                       wxLC_NO_SORT_HEADER)
               .AddListControlReportMode( { XO("Preset") } );
         }
         S.EndStatic();
      }
      S.EndVerticalLay();
   }
   S.EndHorizontalLay();

   S.AddStandardButtons();

   RegistryPaths presets;

   mEffect->mHost->GetPrivateConfigSubgroups(mEffect->mHost->GetUserPresetsGroup(wxEmptyString), presets);

   std::sort( presets.begin(), presets.end() );

   for (size_t i = 0, cnt = presets.size(); i < cnt; i++)
   {
      mList->InsertItem(i, presets[i]);
   }

   // Set the list size...with a little extra for good measure
   wxSize sz = mList->GetBestSize();
   sz.x += 5;
   sz.y += 5;
   mList->SetMinSize(sz);

   Layout();
   Fit();
   Center();

   // Make the single column a reasonable size...not perfect but better
   // than using wxLIST_AUTOSIZE.
   sz = mList->GetSize();
   mList->SetColumnWidth(0, sz.x - 10);
}

wxString AudioUnitEffectExportDialog::Export(const wxString & name)
{
   RegistryPath group = mEffect->mHost->GetUserPresetsGroup(name);

   // Make sure the user preset directory exists
   wxString path;
   path.Printf(wxT("%s/%s/%s/%s.aupreset"),
               PRESET_USER_PATH,
               mEffect->mVendor,
               mEffect->mName,
               name);
   wxFileName fn(path);
   fn.Normalize();
   if (!fn.Mkdir(0755, wxPATH_MKDIR_FULL))
   {
      return _("Couldn't create the \"%s\" directory").Format(fn.GetPath());
   }

   // Create the file
   const wxString fullPath{fn.GetFullPath()};
   wxFFile f(fullPath, wxT("wb"));
   if (!f.IsOpened())
   {
      return _("Couldn't open \"%s\"").Format(fullPath);
   }

   // Retrieve preset from config file
   wxString parms;
   if (!mEffect->mHost->GetPrivateConfig(group, PRESET_KEY, parms, wxEmptyString))
   {
      return _("Preset key \"%s\" not found in group \"%s\"").Format(PRESET_KEY, group);
   }
   
   // Decode it
   wxMemoryBuffer buf = wxBase64Decode(parms);
   size_t bufLen = buf.GetDataLen();
   if (!bufLen)
   {
      return _("Failed to decode preset");
   }
   const uint8_t *bufPtr = (uint8_t *) buf.GetData();

   // Determine if the data is binary or XML
   bool isBin = (bufLen >= 6 && memcmp(bufPtr, "bplist", 6) == 0);

   // Convert binary plist to XML
   if (isBin)
   {
      // Create a CFData object that references the decoded preset
      CFunique_ptr<const __CFData> data
      {
         CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                     bufPtr,
                                     bufLen,
                                     kCFAllocatorNull)
      };
      if (!data)
      {
         return _("Failed to convert preset to internal data representation");
      }

      // Convert it back to a property list.
      CFPropertyListRef content
      {
         CFPropertyListCreateWithData(kCFAllocatorDefault,
                                      data.get(),
                                      kCFPropertyListImmutable,
                                      NULL,
                                      NULL)
      };
      if (!content)
      {
         return _("Failed to create property list from preset data");
      }
      CFunique_ptr<char /* CFPropertyList */> ucontent { (char *) content };

      // Serialize it as XML data
      data.reset(CFPropertyListCreateData(kCFAllocatorDefault,
                                          content,
                                          kCFPropertyListXMLFormat_v1_0,
                                          0,
                                          NULL));
      if (!data)
      {
         return _("Failed to convert property list to XML data");
      }

      // Nothing to do if we don't have any data
      SInt32 length = CFDataGetLength(data.get());
      if (!length)
      {
         return _("XML data is empty after conversion");
      }

      // Write XML data
      if (f.Write(CFDataGetBytePtr(data.get()), length) != length || f.Error())
      {
         return _("Failed to write XML preset to \"%s\"").Format(fullPath);
      }
   }
   else
   {
      // Write XML data
      if (f.Write(bufPtr, bufLen) != bufLen || f.Error())
      {
         return _("Failed to write XML preset to \"%s\"").Format(fullPath);
      }
   }

   f.Close();

   return wxEmptyString;
}

void AudioUnitEffectExportDialog::OnOk(wxCommandEvent & evt)
{
   evt.Skip();

   // Export all selected presets
   long sel = -1;
   while ((sel = mList->GetNextItem(sel, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0)
   {
      wxString name = mList->GetItemText(sel);

      wxString msg = Export(name);

      if (!msg.IsEmpty())
      {
         AudacityMessageBox(
            XO("Could not export \"%s\" preset\n\n%s").Format( name, msg ),
            XO("Export Audio Unit Presets"),
            wxOK | wxCENTRE,
            this);
         return;
      }
   }

   EndModal(wxID_OK);
}

///////////////////////////////////////////////////////////////////////////////
//
// AudioUnitEffectImportDialog
//
///////////////////////////////////////////////////////////////////////////////

class AudioUnitEffectImportDialog final : public wxDialogWrapper
{
public:
   AudioUnitEffectImportDialog(wxWindow * parent, AudioUnitEffect *effect);
   virtual ~AudioUnitEffectImportDialog();

   void PopulateOrExchange(ShuttleGui & S);
   bool HasPresets();
   wxString Import(const wxString & path, const wxString & name);

   void OnOk(wxCommandEvent & evt);

private:
   wxWindow *mParent;
   AudioUnitEffect *mEffect;

   wxListCtrl *mList;

   DECLARE_EVENT_TABLE()
};

BEGIN_EVENT_TABLE(AudioUnitEffectImportDialog, wxDialogWrapper)
   EVT_BUTTON(wxID_OK, AudioUnitEffectImportDialog::OnOk)
END_EVENT_TABLE()

AudioUnitEffectImportDialog::AudioUnitEffectImportDialog(wxWindow * parent, AudioUnitEffect *effect)
:  wxDialogWrapper(parent, wxID_ANY, XO("Import Audio Unit Presets"))
{
   mEffect = effect;

   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
}

AudioUnitEffectImportDialog::~AudioUnitEffectImportDialog()
{
}

void AudioUnitEffectImportDialog::PopulateOrExchange(ShuttleGui & S)
{
   S.SetBorder(5);
   S.StartHorizontalLay(wxEXPAND, 1);
   {
      S.StartVerticalLay(true);
      {
         S.StartStatic(XO("Presets (may select multiple)"));
         {
            mList = S.Style(wxLC_REPORT | wxLC_HRULES | wxLC_VRULES |
                       wxLC_NO_SORT_HEADER)
               .AddListControlReportMode( { XO("Preset"), XO("Location") } );
         }
         S.EndStatic();
      }
      S.EndVerticalLay();
   }
   S.EndHorizontalLay();

   S.AddStandardButtons();

   FilePaths presets;
   wxFileName fn;

   // Generate the local domain path
   wxString path;
   path.Printf(wxT("%s/%s/%s"),
               PRESET_LOCAL_PATH,
               mEffect->mVendor,
               mEffect->mName);
   fn = path;
   fn.Normalize();
   
   // Get all presets in the local domain for this effect
   wxDir::GetAllFiles(fn.GetFullPath(), &presets, wxT("*.aupreset"));

   // Generate the user domain path
   path.Printf(wxT("%s/%s/%s"),
               PRESET_USER_PATH,
               mEffect->mVendor,
               mEffect->mName);
   fn = path;
   fn.Normalize();

   // Get all presets in the user domain for this effect
   wxDir::GetAllFiles(fn.GetFullPath(), &presets, wxT("*.aupreset"));
   
   presets.Sort();

   for (size_t i = 0, cnt = presets.size(); i < cnt; i++)
   {
      fn = presets[i];
      mList->InsertItem(i, fn.GetName());
      mList->SetItem(i, 1, fn.GetPath());
   }

   mList->SetColumnWidth(0, wxLIST_AUTOSIZE);
   mList->SetColumnWidth(1, wxLIST_AUTOSIZE);

   // Set the list size...with a little extra for good measure
   wxSize sz = mList->GetBestSize();
   sz.x += 5;
   sz.y += 5;
   mList->SetMinSize(sz);

   Layout();
   Fit();
   Center();
}

bool AudioUnitEffectImportDialog::HasPresets()
{
   return mList->GetItemCount() > 0;
}

wxString AudioUnitEffectImportDialog::Import(const wxString & path, const wxString & name)
{
   // Generate the path
   wxString fullPath;
   fullPath.Printf(wxT("%s/%s.aupreset"),
                    path,
                    name);

   // Open the preset
   wxFFile f(fullPath, wxT("r"));
   if (!f.IsOpened())
   {
      return wxString::Format(_("Couldn't open \"%s\""), fullPath);
   }

   // Load it into the buffer
   size_t len = f.Length();
   wxMemoryBuffer buf(len);
   if (f.Read(buf.GetData(), len) != len || f.Error())
   {
      return wxString::Format(_("Unable to read the preset from \"%s\""), fullPath);
   }

   wxString parms = wxBase64Encode(buf.GetData(), len);
   if (parms.IsEmpty())
   {
      return wxString::Format(_("Failed to encode preset from \"%s\""), fullPath);
   }

   // And write it to the config
   wxString group = mEffect->mHost->GetUserPresetsGroup(name);
   if (!mEffect->mHost->SetPrivateConfig(group, PRESET_KEY, parms))
   {
      return wxString::Format(_("Unable to store preset in config file"));
   }

   return wxEmptyString;
}

void AudioUnitEffectImportDialog::OnOk(wxCommandEvent & evt)
{
   evt.Skip();
   
   // Import all selected presets
   long sel = -1;
   while ((sel = mList->GetNextItem(sel, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0)
   {
      wxListItem item;
      item.SetId(sel);
      item.SetColumn(1);
      item.SetMask(wxLIST_MASK_TEXT);
      mList->GetItem(item);

      wxString path = item.GetText();
      wxString name = mList->GetItemText(sel);
      wxString msg = Import(path, name);

      if (!msg.IsEmpty())
      {
         AudacityMessageBox(
            XO("Could not import \"%s\" preset\n\n%s").Format( name, msg ),
            XO("Import Audio Unit Presets"),
            wxOK | wxCENTRE,
            this);
         return;
      }
   }
  
   EndModal(wxID_OK);
}

///////////////////////////////////////////////////////////////////////////////
//
// AudioUnitEffect
//
///////////////////////////////////////////////////////////////////////////////

AudioUnitEffect::AudioUnitEffect(const PluginPath & path,
                                 const wxString & name,
                                 AudioComponent component,
                                 AudioUnitEffect *master)
{
   mPath = path;
   mName = name.AfterFirst(wxT(':')).Trim(true).Trim(false);
   mVendor = name.BeforeFirst(wxT(':')).Trim(true).Trim(false);
   mComponent = component;
   mMaster = master;

   mUnit = NULL;
   
   mBlockSize = 0.0;
   mInteractive = false;
   mIsGraphical = false;

   mUIHost = NULL;
   mDialog = NULL;
   mParent = NULL;

   mUnitInitialized = false;

   mEventListenerRef = NULL;
}

AudioUnitEffect::~AudioUnitEffect()
{
   if (mUnitInitialized)
   {
      AudioUnitUninitialize(mUnit);
   }

   if (mEventListenerRef)
   {
      AUListenerDispose(mEventListenerRef);
   }

   if (mUnit)
   {
      AudioComponentInstanceDispose(mUnit);
   }
}

// ============================================================================
// ComponentInterface implementation
// ============================================================================

PluginPath AudioUnitEffect::GetPath()
{
   return mPath;
}

ComponentInterfaceSymbol AudioUnitEffect::GetSymbol()
{
   return mName;
}

VendorSymbol AudioUnitEffect::GetVendor()
{
   return { mVendor };
}

wxString AudioUnitEffect::GetVersion()
{
   UInt32 version;

   OSStatus result = AudioComponentGetVersion(mComponent, &version);

   return wxString::Format(wxT("%d.%d.%d"),
                           (version >> 16) & 0xffff,
                           (version >> 8) & 0xff,
                           version & 0xff);
}

TranslatableString AudioUnitEffect::GetDescription()
{
   /* i18n-hint: Can mean "not available," "not applicable," "no answer" */
   return XO("n/a");
}

// ============================================================================
// EffectComponentInterface implementation
// ============================================================================

EffectType AudioUnitEffect::GetType()
{
   if (mAudioIns == 0 && mAudioOuts == 0)
   {
      return EffectTypeNone;
   }

   if (mAudioIns == 0)
   {
      return EffectTypeGenerate;
   }

   if (mAudioOuts == 0)
   {
      return EffectTypeAnalyze;
   }

   return EffectTypeProcess;
}

EffectFamilySymbol AudioUnitEffect::GetFamily()
{
   return AUDIOUNITEFFECTS_FAMILY;
}

bool AudioUnitEffect::IsInteractive()
{
   return mInteractive;
}

bool AudioUnitEffect::IsDefault()
{
   return false;
}

bool AudioUnitEffect::IsLegacy()
{
   return false;
}

bool AudioUnitEffect::SupportsRealtime()
{
   return GetType() == EffectTypeProcess;
}

bool AudioUnitEffect::SupportsAutomation()
{
   OSStatus result;
   UInt32 dataSize;
   Boolean isWritable;

   result = AudioUnitGetPropertyInfo(mUnit,
                                     kAudioUnitProperty_ParameterList,
                                     kAudioUnitScope_Global,
                                     0,
                                     &dataSize,
                                     &isWritable);
   if (result != noErr)
   {
      return false;
   }

   UInt32 cnt = dataSize / sizeof(AudioUnitParameterID);
   ArrayOf<AudioUnitParameterID> array{cnt};

   result = AudioUnitGetProperty(mUnit,
                                 kAudioUnitProperty_ParameterList,
                                 kAudioUnitScope_Global,
                                 0,
                                 array.get(),
                                 &dataSize);  
   if (result != noErr)
   {
      return false;
   }

   for (int i = 0; i < cnt; i++)
   {
      ParameterInfo pi;

      if (pi.Get(mUnit, array[i]))
      {
         if (pi.info.flags & kAudioUnitParameterFlag_IsWritable)
         {
            // All we need is one
            return true;
         }
      }
   }

   return false;
}

// ============================================================================
// EffectClientInterface Implementation
// ============================================================================

bool AudioUnitEffect::SetHost(EffectHostInterface *host)
{
   OSStatus result;
   
   mHost = host;
 
   mSampleRate = 44100;
   result = AudioComponentInstanceNew(mComponent, &mUnit);
   if (!mUnit)
   {
      return false;
   }

   GetChannelCounts();

   SetRateAndChannels();

   // Retrieve the desired number of frames per slice
   UInt32 dataSize = sizeof(mBlockSize);
   mBlockSize = 512;
   AudioUnitGetProperty(mUnit,
                        kAudioUnitProperty_MaximumFramesPerSlice,
                        kAudioUnitScope_Global,
                        0,
                        &mBlockSize,
                        &dataSize);

   // mHost will be null during registration
   if (mHost)
   {
      mHost->GetSharedConfig(wxT("Options"), wxT("UseLatency"), mUseLatency, true);
      mHost->GetSharedConfig(wxT("Options"), wxT("UIType"), mUIType, wxT("Full"));

      bool haveDefaults;
      mHost->GetPrivateConfig(mHost->GetFactoryDefaultsGroup(), wxT("Initialized"), haveDefaults, false);
      if (!haveDefaults)
      {
         SavePreset(mHost->GetFactoryDefaultsGroup());
         mHost->SetPrivateConfig(mHost->GetFactoryDefaultsGroup(), wxT("Initialized"), true);
      }

      LoadPreset(mHost->GetCurrentSettingsGroup());
   } 

   if (!mMaster)
   {
     result = AUEventListenerCreate(AudioUnitEffect::EventListenerCallback,
                                    this,
                                    (CFRunLoopRef)GetCFRunLoopFromEventLoop(GetCurrentEventLoop()),
                                    kCFRunLoopDefaultMode,
                                    0.0,
                                    0.0,
                                    &mEventListenerRef);
      if (result != noErr)
      {
         return false;
      }

      AudioUnitEvent event;
 
      event.mEventType = kAudioUnitEvent_ParameterValueChange;
      event.mArgument.mParameter.mAudioUnit = mUnit;
      event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
      event.mArgument.mParameter.mElement = 0;

      UInt32 dataSize;
      Boolean isWritable;

      // Retrieve the list of parameters
      result = AudioUnitGetPropertyInfo(mUnit,
                                        kAudioUnitProperty_ParameterList,
                                        kAudioUnitScope_Global,
                                        0,
                                        &dataSize,
                                        &isWritable);
      if (result != noErr)
      {
         return false;
      }

      // And get them
      UInt32 cnt = dataSize / sizeof(AudioUnitParameterID);
      if (cnt != 0)
      {
         ArrayOf<AudioUnitParameterID> array {cnt};

         result = AudioUnitGetProperty(mUnit,
                                       kAudioUnitProperty_ParameterList,
                                       kAudioUnitScope_Global,
                                       0,
                                       array.get(),
                                       &dataSize);  
         if (result != noErr)
         {
            return false;
         }

         // Register them as something we're interested in
         for (int i = 0; i < cnt; i++)
         {
            event.mArgument.mParameter.mParameterID = array[i];
            result = AUEventListenerAddEventType(mEventListenerRef,
                                                 this,
                                                 &event);
            if (result != noErr)
            {
               return false;
            }
         }
      }

      event.mEventType = kAudioUnitEvent_PropertyChange;
      event.mArgument.mProperty.mAudioUnit = mUnit;
      event.mArgument.mProperty.mPropertyID = kAudioUnitProperty_Latency;
      event.mArgument.mProperty.mScope = kAudioUnitScope_Global;
      event.mArgument.mProperty.mElement = 0;

      result = AUEventListenerAddEventType(mEventListenerRef,
                                           this,
                                           &event);
      if (result != noErr)
      {
         return false;
      }

      AudioUnitCocoaViewInfo cocoaViewInfo;
      dataSize = sizeof(AudioUnitCocoaViewInfo);
   
      // Check for a Cocoa UI
      result = AudioUnitGetProperty(mUnit,
                                    kAudioUnitProperty_CocoaUI,
                                    kAudioUnitScope_Global,
                                    0,
                                    &cocoaViewInfo,
                                    &dataSize);

      bool hasCocoa = result == noErr;

      // Check for a Carbon UI
      AudioComponentDescription compDesc;
      dataSize = sizeof(compDesc);
      result = AudioUnitGetProperty(mUnit,
                                    kAudioUnitProperty_GetUIComponentList,
                                    kAudioUnitScope_Global,
                                    0,
                                    &compDesc,
                                    &dataSize);
      bool hasCarbon = result == noErr;

      mInteractive = (cnt > 0) || hasCocoa || hasCarbon;
   }

   return true;
}

unsigned AudioUnitEffect::GetAudioInCount()
{
   return mAudioIns;
}

unsigned AudioUnitEffect::GetAudioOutCount()
{
   return mAudioOuts;
}

int AudioUnitEffect::GetMidiInCount()
{
   return 0;
}

int AudioUnitEffect::GetMidiOutCount()
{
   return 0;
}

void AudioUnitEffect::SetSampleRate(double rate)
{
   mSampleRate = rate;
}

size_t AudioUnitEffect::SetBlockSize(size_t maxBlockSize)
{
   return mBlockSize;
}

size_t AudioUnitEffect::GetBlockSize() const
{
   return mBlockSize;
}

sampleCount AudioUnitEffect::GetLatency()
{
   // Retrieve the latency (can be updated via an event)
   if (mUseLatency && !mLatencyDone)
   {
      mLatencyDone = true;

      Float64 latency = 0.0;
      UInt32 dataSize = sizeof(latency);
      AudioUnitGetProperty(mUnit,
                           kAudioUnitProperty_Latency,
                           kAudioUnitScope_Global,
                           0,
                           &latency,
                           &dataSize);  

      return sampleCount( latency * mSampleRate );
   }

   return 0;
}

size_t AudioUnitEffect::GetTailSize()
{
   // Retrieve the tail time
   Float64 tailTime = 0.0;
   UInt32 dataSize = sizeof(tailTime);
   AudioUnitGetProperty(mUnit,
                        kAudioUnitProperty_TailTime,
                        kAudioUnitScope_Global,
                        0,
                        &tailTime,
                        &dataSize);  

   return tailTime * mSampleRate;
}

bool AudioUnitEffect::IsReady()
{
   return mReady;
}

bool AudioUnitEffect::ProcessInitialize(sampleCount WXUNUSED(totalLen), ChannelNames WXUNUSED(chanMap))
{
   OSStatus result;

   mInputList.reinit( mAudioIns );
   mInputList[0].mNumberBuffers = mAudioIns;
   
   mOutputList.reinit( mAudioOuts );
   mOutputList[0].mNumberBuffers = mAudioOuts;

   memset(&mTimeStamp, 0, sizeof(AudioTimeStamp));
   mTimeStamp.mSampleTime = 0; // This is a double-precision number that should
                               // accumulate the number of frames processed so far
   mTimeStamp.mFlags = kAudioTimeStampSampleTimeValid;

   if (!SetRateAndChannels())
   {
      return false;
   }

   AURenderCallbackStruct callbackStruct;
   callbackStruct.inputProc = RenderCallback;
   callbackStruct.inputProcRefCon = this;
   result = AudioUnitSetProperty(mUnit,
                                 kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Input,
                                 0,
                                 &callbackStruct,
                                 sizeof(AURenderCallbackStruct));
   if (result != noErr)
   {
      wxPrintf("Setting input render callback failed.\n");
      return false;
   }

   result = AudioUnitReset(mUnit, kAudioUnitScope_Global, 0);
   if (result != noErr)
   {
      return false;
   }

   mLatencyDone = false;

   mReady = true;

   return true;
}

bool AudioUnitEffect::ProcessFinalize()
{
   mReady = false;

   mOutputList.reset();
   mInputList.reset();

   return true;
}

size_t AudioUnitEffect::ProcessBlock(float **inBlock, float **outBlock, size_t blockLen)
{
   for (size_t i = 0; i < mAudioIns; i++)
   {
      mInputList[0].mBuffers[i].mNumberChannels = 1;
      mInputList[0].mBuffers[i].mData = inBlock[i];
      mInputList[0].mBuffers[i].mDataByteSize = sizeof(float) * blockLen;
   }

   for (size_t i = 0; i < mAudioOuts; i++)
   {
      mOutputList[0].mBuffers[i].mNumberChannels = 1;
      mOutputList[0].mBuffers[i].mData = outBlock[i];
      mOutputList[0].mBuffers[i].mDataByteSize = sizeof(float) * blockLen;
   }

   AudioUnitRenderActionFlags flags = 0;
   OSStatus result;

   result = AudioUnitRender(mUnit,
                            &flags,
                            &mTimeStamp,
                            0,
                            blockLen,
                            mOutputList.get());
   if (result != noErr)
   {
      wxPrintf("Render failed: %d %4.4s\n", (int)result, (char *)&result);
      return 0;
   }

   mTimeStamp.mSampleTime += blockLen;

   return blockLen;
}

bool AudioUnitEffect::RealtimeInitialize()
{
   mMasterIn.reinit(mAudioIns, mBlockSize, true);
   mMasterOut.reinit( mAudioOuts, mBlockSize );
   return ProcessInitialize(0);
}

bool AudioUnitEffect::RealtimeAddProcessor(unsigned numChannels, float sampleRate)
{
   auto slave = std::make_unique<AudioUnitEffect>(mPath, mName, mComponent, this);
   if (!slave->SetHost(NULL))
      return false;

   slave->SetBlockSize(mBlockSize);
   slave->SetChannelCount(numChannels);
   slave->SetSampleRate(sampleRate);

   if (!CopyParameters(mUnit, slave->mUnit))
      return false;

   auto pSlave = slave.get();
   mSlaves.push_back(std::move(slave));

   return pSlave->ProcessInitialize(0);
}

bool AudioUnitEffect::RealtimeFinalize()
{
   for (size_t i = 0, cnt = mSlaves.size(); i < cnt; i++)
      mSlaves[i]->ProcessFinalize();
   mSlaves.clear();

   mMasterIn.reset();
   mMasterOut.reset();

   return ProcessFinalize();
}

bool AudioUnitEffect::RealtimeSuspend()
{
   return true;
}

bool AudioUnitEffect::RealtimeResume()
{
   OSStatus result;

   result = AudioUnitReset(mUnit, kAudioUnitScope_Global, 0);
   if (result != noErr)
   {
      return false;
   }

   return true;
}

bool AudioUnitEffect::RealtimeProcessStart()
{
   for (size_t i = 0; i < mAudioIns; i++)
      memset(mMasterIn[i].get(), 0, mBlockSize * sizeof(float));

   mNumSamples = 0;

   return true;
}

size_t AudioUnitEffect::RealtimeProcess(int group,
                                        float **inbuf,
                                        float **outbuf,
                                        size_t numSamples)
{
   wxASSERT(numSamples <= mBlockSize);

   for (size_t c = 0; c < mAudioIns; c++)
   {
      for (decltype(numSamples) s = 0; s < numSamples; s++)
      {
         mMasterIn[c][s] += inbuf[c][s];
      }
   }
   mNumSamples = wxMax(numSamples, mNumSamples);

   return mSlaves[group]->ProcessBlock(inbuf, outbuf, numSamples);
}

bool AudioUnitEffect::RealtimeProcessEnd()
{
   ProcessBlock(
      reinterpret_cast<float**>(mMasterIn.get()),
      reinterpret_cast<float**>(mMasterOut.get()),
      mNumSamples);

   return true;
}

bool AudioUnitEffect::ShowInterface(
   wxWindow &parent, const EffectDialogFactory &factory, bool forceModal)
{
   if (mDialog)
   {
      if( mDialog->Close(true) )
         mDialog = nullptr;
      return false;
   }

   // mDialog is null
   auto cleanup = valueRestorer( mDialog );

   if ( factory )
      mDialog = factory(parent, mHost, this);
   if (!mDialog)
   {
      return false;
   }

   if ((SupportsRealtime() || GetType() == EffectTypeAnalyze) && !forceModal)
   {
      mDialog->Show();
      cleanup.release();

      return false;
   }

   bool res = mDialog->ShowModal() != 0;

   return res;
}

bool AudioUnitEffect::GetAutomationParameters(CommandParameters & parms)
{
   OSStatus result;
   UInt32 dataSize;
   Boolean isWritable;

   result = AudioUnitGetPropertyInfo(mUnit,
                                     kAudioUnitProperty_ParameterList,
                                     kAudioUnitScope_Global,
                                     0,
                                     &dataSize,
                                     &isWritable);
   if (result != noErr)
   {
      return false;
   }

   UInt32 cnt = dataSize / sizeof(AudioUnitParameterID);
   ArrayOf<AudioUnitParameterID> array {cnt};

   result = AudioUnitGetProperty(mUnit,
                                 kAudioUnitProperty_ParameterList,
                                 kAudioUnitScope_Global,
                                 0,
                                 array.get(),
                                 &dataSize);  
   if (result != noErr)
   {
      return false;
   }

   for (int i = 0; i < cnt; i++)
   {
      ParameterInfo pi;

      if (!pi.Get(mUnit, array[i]))
      {
         // Probably failed because of invalid parameter which can happen
         // if a plug-in is in a certain mode that doesn't contain the
         // parameter.  In any case, just ignore it.
         continue;
      }

      AudioUnitParameterValue value;
      result = AudioUnitGetParameter(mUnit,
                                     array[i],
                                     kAudioUnitScope_Global,
                                     0,
                                     &value);
      if (result != noErr)
      {
         // Probably failed because of invalid parameter which can happen
         // if a plug-in is in a certain mode that doesn't contain the
         // parameter.  In any case, just ignore it.
         continue;
      }

      parms.Write(pi.name, value);
   }

   return true;
}

bool AudioUnitEffect::SetAutomationParameters(CommandParameters & parms)
{
   OSStatus result;
   UInt32 dataSize;
   Boolean isWritable;

   result = AudioUnitGetPropertyInfo(mUnit,
                                     kAudioUnitProperty_ParameterList,
                                     kAudioUnitScope_Global,
                                     0,
                                     &dataSize,
                                     &isWritable);
   if (result != noErr)
   {
      return false;
   }

   UInt32 cnt = dataSize / sizeof(AudioUnitParameterID);
   ArrayOf<AudioUnitParameterID> array {cnt};

   result = AudioUnitGetProperty(mUnit,
                                 kAudioUnitProperty_ParameterList,
                                 kAudioUnitScope_Global,
                                 0,
                                 array.get(),
                                 &dataSize);  
   if (result != noErr)
   {
      return false;
   }

   for (int i = 0; i < cnt; i++)
   {
      ParameterInfo pi;

      if (!pi.Get(mUnit, array[i]))
      {
         // Probably failed because of invalid parameter which can happen
         // if a plug-in is in a certain mode that doesn't contain the
         // parameter.  In any case, just ignore it.
         continue;
      }

      double d = 0.0;
      if (parms.Read(pi.name, &d))
      {
         AudioUnitParameterValue value = d;
         AudioUnitSetParameter(mUnit,
                               array[i],
                               kAudioUnitScope_Global,
                               0,
                               value,
                               0);

         AudioUnitParameter aup = {};
         aup.mAudioUnit = mUnit;
         aup.mParameterID = array[i];
         aup.mScope = kAudioUnitScope_Global;
         aup.mElement = 0;
         AUParameterListenerNotify(NULL, NULL, &aup);
      }
   }

   return true;
}

bool AudioUnitEffect::LoadUserPreset(const RegistryPath & name)
{
   return LoadPreset(name);
}

bool AudioUnitEffect::SaveUserPreset(const RegistryPath & name)
{
   return SavePreset(name);
}

bool AudioUnitEffect::LoadFactoryPreset(int id)
{
   OSStatus result;

   // Retrieve the list of factory presets
   CFArrayRef array{};
   UInt32 dataSize = sizeof(CFArrayRef);
   result = AudioUnitGetProperty(mUnit,
                                 kAudioUnitProperty_FactoryPresets,
                                 kAudioUnitScope_Global,
                                 0,
                                 &array,
                                 &dataSize);
   CFunique_ptr<const __CFArray> uarray { array };
   if (result != noErr)
   {
      return false;
   }

   if (id < 0 || id >= CFArrayGetCount(array))
   {
      return false;
   }

   AUPreset *preset = (AUPreset *) CFArrayGetValueAtIndex(array, id);

   result = AudioUnitSetProperty(mUnit,
                                 kAudioUnitProperty_PresentPreset,
                                 kAudioUnitScope_Global,
                                 0,
                                 preset,
                                 sizeof(AUPreset));
   if (result == noErr)
   {
      AudioUnitParameter aup;
      aup.mAudioUnit = mUnit;
      aup.mParameterID = kAUParameterListener_AnyParameter;
      aup.mScope = kAudioUnitScope_Global;
      aup.mElement = 0;
      AUParameterListenerNotify(NULL, NULL, &aup);
   }

   return result == noErr;
}

bool AudioUnitEffect::LoadFactoryDefaults()
{
   return LoadPreset(mHost->GetFactoryDefaultsGroup());
}

RegistryPaths AudioUnitEffect::GetFactoryPresets()
{
   OSStatus result;
   RegistryPaths presets;

   // Retrieve the list of factory presets
   CFArrayRef array{};
   UInt32 dataSize = sizeof(CFArrayRef);
   result = AudioUnitGetProperty(mUnit,
                                 kAudioUnitProperty_FactoryPresets,
                                 kAudioUnitScope_Global,
                                 0,
                                 &array,
                                 &dataSize);
   CFunique_ptr<const __CFArray> uarray { array };
   if (result == noErr)
   {
      for (CFIndex i = 0, cnt = CFArrayGetCount(array); i < cnt; i++)
      {
         AUPreset *preset = (AUPreset *) CFArrayGetValueAtIndex(array, i);
         presets.push_back(wxCFStringRef::AsString(preset->presetName));
      }
   }
                        
   return presets;
}

// ============================================================================
// EffectUIClientInterface Implementation
// ============================================================================

void AudioUnitEffect::SetHostUI(EffectUIHostInterface *host)
{
   mUIHost = host;
}

bool AudioUnitEffect::PopulateUI(ShuttleGui &S)
{
   // OSStatus result;

   auto parent = S.GetParent();
   mDialog = static_cast<wxDialog *>(wxGetTopLevelParent(parent));
   mParent = parent;

   wxPanel *container;
   {
      auto mainSizer = std::make_unique<wxBoxSizer>(wxVERTICAL);

      wxASSERT(mParent); // To justify safenew
      container = safenew wxPanelWrapper(mParent, wxID_ANY);
      mainSizer->Add(container, 1, wxEXPAND);

      mParent->SetSizer(mainSizer.release());
   }

   if (mUIType == wxT("Basic"))
   {
      if (!CreatePlain(mParent))
      {
         return false;
      }
   }
   else
   {
      auto pControl = Destroy_ptr<AUControl>( safenew AUControl );
      if (!pControl)
      {
         return false;
      }

      if (!pControl->Create(container, mComponent, mUnit, mUIType == wxT("Full")))
      {
         return false;
      }

      {
         auto innerSizer = std::make_unique<wxBoxSizer>(wxVERTICAL);

         innerSizer->Add((mpControl = pControl.release()), 1, wxEXPAND);
         container->SetSizer(innerSizer.release());
      }

      mParent->SetMinSize(wxDefaultSize);

#ifdef __WXMAC__
#ifdef __WX_EVTLOOP_BUSY_WAITING__
      wxEventLoop::SetBusyWaiting(true);
#endif
#endif
   }

   mParent->PushEventHandler(this);

   return true;
}

bool AudioUnitEffect::IsGraphicalUI()
{
   return mUIType != wxT("Plain");
}

bool AudioUnitEffect::ValidateUI()
{
#if 0
   if (!mParent->Validate())
   {
      return false;
   }

   if (GetType() == EffectTypeGenerate)
   {
      mHost->SetDuration(mDuration->GetValue());
   }
#endif
   return true;
}

bool AudioUnitEffect::CreatePlain(wxWindow *parent)
{
   // TODO???  Never implemented...
   return false;
}

bool AudioUnitEffect::HideUI()
{
#if 0
   if (GetType() == EffectTypeAnalyze || mNumOutputControls > 0)
   {
      return false;
   }
#endif
   return true;
}

bool AudioUnitEffect::CloseUI()
{
#ifdef __WXMAC__
#ifdef __WX_EVTLOOP_BUSY_WAITING__
   wxEventLoop::SetBusyWaiting(false);
#endif
   if (mpControl)
   {
      mpControl->Close();
      mpControl = nullptr;
   }
#endif

   mParent->RemoveEventHandler(this);

   mUIHost = NULL;
   mParent = NULL;
   mDialog = NULL;

   return true;
}

bool AudioUnitEffect::CanExportPresets()
{
   return true;
}

void AudioUnitEffect::ExportPresets()
{
   RegistryPaths presets;

   mHost->GetPrivateConfigSubgroups(mHost->GetUserPresetsGroup(wxEmptyString), presets);

   if (presets.size())
   { 
      AudioUnitEffectExportDialog dlg(mDialog, this);
      dlg.ShowModal();
   }
   else
   {
      AudacityMessageBox(XO("No user presets to export."),
                         XO("Export Presets"),
                         wxOK | wxCENTRE,
                         mDialog);
   }
}

void AudioUnitEffect::ImportPresets()
{
   AudioUnitEffectImportDialog dlg(mDialog, this);
   if (dlg.HasPresets())
   {
      dlg.ShowModal();
   }
   else
   {
      AudacityMessageBox(XO("No user or local presets to import."),
                         XO("Import Presets"),
                         wxOK | wxCENTRE,
                         mDialog);
   }
}

bool AudioUnitEffect::HasOptions()
{
   return true;
}

void AudioUnitEffect::ShowOptions()
{
   AudioUnitEffectOptionsDialog dlg(mParent, mHost);
   if (dlg.ShowModal())
   {
      // Reinitialize configuration settings
      mHost->GetSharedConfig(wxT("Options"), wxT("UseLatency"), mUseLatency, true);
      mHost->GetSharedConfig(wxT("Options"), wxT("UIType"), mUIType, wxT("Full"));
   }
}

// ============================================================================
// AudioUnitEffect Implementation
// ============================================================================

bool AudioUnitEffect::LoadPreset(const RegistryPath & group)
{
   wxString parms;

   // Attempt to load old preset parameters and resave using new method
   if (mHost->GetPrivateConfig(group, wxT("Parameters"), parms, wxEmptyString))
   {
      CommandParameters eap;
      if (eap.SetParameters(parms))
      {
         if (SetAutomationParameters(eap))
         {
            if (SavePreset(group))
            {
               mHost->RemovePrivateConfig(group, wxT("Parameters"));
            }
         }
      }
      return true;
   }

   // Retrieve the preset
   if (!mHost->GetPrivateConfig(group, PRESET_KEY, parms, wxEmptyString))
   {
      // Commented "CurrentSettings" gets tried a lot and useless messages appear
      // in the log
      //wxLogError(wxT("Preset key \"%s\" not found in group \"%s\""), PRESET_KEY, group);
      return false;
   }
   
   // Decode it
   wxMemoryBuffer buf = wxBase64Decode(parms);
   size_t bufLen = buf.GetDataLen();
   if (!bufLen)
   {
      wxLogError(wxT("Failed to decode \"%s\" preset"), group);
      return false;
   }
   const uint8_t *bufPtr = (uint8_t *) buf.GetData();

   // Create a CFData object that references the decoded preset
   CFunique_ptr<const __CFData> data
   {
      CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                  bufPtr,
                                  bufLen,
                                  kCFAllocatorNull)
   };
   if (!data)
   {
      wxLogError(wxT("Failed to convert \"%s\" preset to internal format"), group);
      return false;
   }

   // Convert it back to a property list.
   CFPropertyListRef content
   {
      CFPropertyListCreateWithData(kCFAllocatorDefault,
                                   data.get(),
                                   kCFPropertyListImmutable,
                                   NULL,
                                   NULL)
   };
   if (!content)
   {
      wxLogError(wxT("Failed to create property list for \"%s\" preset"), group);
      return false;
   }
   CFunique_ptr<char /* CFPropertyList */> ucontent { (char *) content };

   // Finally, update the properties and parameters
   OSStatus result;
   result = AudioUnitSetProperty(mUnit,
                                 kAudioUnitProperty_ClassInfo,
                                 kAudioUnitScope_Global,
                                 0,
                                 &content,
                                 sizeof(content));
   if (result != noErr)
   {
      wxLogError(wxT("Failed to set class info for \"%s\" preset"), group);
      return false;
   }

   // And notify any interested parties
   AudioUnitParameter aup = {};
   aup.mAudioUnit = mUnit;
   aup.mParameterID = kAUParameterListener_AnyParameter;
   aup.mScope = kAudioUnitScope_Global;
   aup.mElement = 0;
   AUParameterListenerNotify(NULL, NULL, &aup);

   // Make sure all slaves get the new preset as well
   for (size_t i = 0, cnt = mSlaves.size(); i < cnt; i++)
   {
      // Finally, update the properties and parameters
      OSStatus result;
      result = AudioUnitSetProperty(mSlaves[i]->mUnit,
                                    kAudioUnitProperty_ClassInfo,
                                    kAudioUnitScope_Global,
                                    0,
                                    &content,
                                    sizeof(content));
      if (result != noErr)
      {
         wxLogError(wxT("Failed to set slave class info for \"%s\" preset"), group);
      }

      // And notify any interested parties
      AudioUnitParameter aup = {};
      aup.mAudioUnit = mUnit;
      aup.mParameterID = kAUParameterListener_AnyParameter;
      aup.mScope = kAudioUnitScope_Global;
      aup.mElement = 0;
      AUParameterListenerNotify(NULL, NULL, &aup);
   }

   return true;
}

bool AudioUnitEffect::SavePreset(const RegistryPath & group)
{
   // First set the name of the preset
   wxCFStringRef cfname(wxFileNameFromPath(group));

   // Define the preset property
   AUPreset preset;
   preset.presetNumber = -1; // indicates user preset
   preset.presetName = cfname;

   // And set it in the audio unit
   AudioUnitSetProperty(mUnit,
                        kAudioUnitProperty_PresentPreset,
                        kAudioUnitScope_Global,
                        0,
                        &preset,
                        sizeof(preset));

   // Now retrieve the preset content
   CFPropertyListRef content;
   UInt32 size = sizeof(content);
   AudioUnitGetProperty(mUnit,
                        kAudioUnitProperty_ClassInfo,
                        kAudioUnitScope_Global,
                        0,
                        &content,
                        &size);
   CFunique_ptr<char /* CFPropertyList */> ucontent { (char *) content };

   // And convert it to serialized binary data
   CFunique_ptr<const __CFData> data
   {
      CFPropertyListCreateData(kCFAllocatorDefault,
                               content,
                               PRESET_FORMAT,
                               0,
                               NULL)
   };
   if (!data)
   {
      return false;
   }

   // Nothing to do if we don't have any data
   SInt32 length = CFDataGetLength(data.get());
   if (length)
   {
      // Base64 encode the returned binary property list
      wxString parms = wxBase64Encode(CFDataGetBytePtr(data.get()), length);

      // And write it to the config
      if (!mHost->SetPrivateConfig(group, PRESET_KEY, parms))
      {
         return false;
      }
   }

   return true;
}

bool AudioUnitEffect::SetRateAndChannels()
{
   OSStatus result;

   if (mUnitInitialized)
   {
      AudioUnitUninitialize(mUnit);

      mUnitInitialized = false;
   }

   AudioStreamBasicDescription streamFormat {
      // Float64 mSampleRate;
      mSampleRate,

      // UInt32  mFormatID;
      kAudioFormatLinearPCM,

      // UInt32  mFormatFlags;
      (kAudioFormatFlagsNativeFloatPacked |
          kAudioFormatFlagIsNonInterleaved),

      // UInt32  mBytesPerPacket;
      sizeof(float),

      // UInt32  mFramesPerPacket;
      1,

      // UInt32  mBytesPerFrame;
      sizeof(float),

      // UInt32  mChannelsPerFrame;
      mAudioIns,

      // UInt32  mBitsPerChannel;
      sizeof(float) * 8,

      // UInt32  mReserved;
      0
   };

   result = AudioUnitSetProperty(mUnit,
                                 kAudioUnitProperty_SampleRate,
                                 kAudioUnitScope_Global,
                                 0,
                                 &mSampleRate,
                                 sizeof(Float64));
   if (result != noErr)
   {
      wxPrintf("%ls Didn't accept sample rate on global\n",
               // Exposing internal name only in debug printf
               GetSymbol().Internal().wx_str());
      return false;
   }

   if (mAudioIns > 0)
   {
      result = AudioUnitSetProperty(mUnit,
                                    kAudioUnitProperty_SampleRate,
                                    kAudioUnitScope_Input,
                                    0,
                                    &mSampleRate,
                                    sizeof(Float64));
      if (result != noErr)
      {
         wxPrintf("%ls Didn't accept sample rate on input\n",
               // Exposing internal name only in debug printf
               GetSymbol().Internal().wx_str());
         return false;
      }

      result = AudioUnitSetProperty(mUnit,
                                    kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Input,
                                    0,
                                    &streamFormat,
                                    sizeof(AudioStreamBasicDescription));
      if (result != noErr)
      {
         wxPrintf("%ls didn't accept stream format on input\n",
               // Exposing internal name only in debug printf
               GetSymbol().Internal().wx_str());
         return false;
      }
   }

   if (mAudioOuts > 0)
   {
      result = AudioUnitSetProperty(mUnit,
                                    kAudioUnitProperty_SampleRate,
                                    kAudioUnitScope_Output,
                                    0,
                                    &mSampleRate,
                                    sizeof(Float64));
      if (result != noErr)
      {
         wxPrintf("%ls Didn't accept sample rate on output\n",
               // Exposing internal name only in debug printf
               GetSymbol().Internal().wx_str());
         return false;
      }
   
      streamFormat.mChannelsPerFrame = mAudioOuts;
      result = AudioUnitSetProperty(mUnit,
                                    kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output,
                                    0,
                                    &streamFormat,
                                    sizeof(AudioStreamBasicDescription));
   
      if (result != noErr)
      {
         wxPrintf("%ls didn't accept stream format on output\n",
               // Exposing internal name only in debug printf
               GetSymbol().Internal().wx_str());
         return false;
      }
   }

   result = AudioUnitInitialize(mUnit);
   if (result != noErr)
   {
      wxPrintf("Couldn't initialize audio unit\n");
      return false;
   }

   mUnitInitialized = true;

   return true;
}

bool AudioUnitEffect::CopyParameters(AudioUnit srcUnit, AudioUnit dstUnit)
{
   OSStatus result;

   // Retrieve the class state from the source AU
   CFPropertyListRef content;
   UInt32 size = sizeof(content);
   result = AudioUnitGetProperty(srcUnit,
                                 kAudioUnitProperty_ClassInfo,
                                 kAudioUnitScope_Global,
                                 0,
                                 &content,
                                 &size);
   if (result != noErr)
   {
      return false;
   }

   // Make sure it get's freed
   CFunique_ptr<char /* CFPropertyList */> ucontent { (char *) content };

   // Set the destination AUs state from the source AU's content
   result = AudioUnitSetProperty(dstUnit,
                                 kAudioUnitProperty_ClassInfo,
                                 kAudioUnitScope_Global,
                                 0,
                                 &content,
                                 sizeof(content));
   if (result != noErr)
   {
      return false;
   }

   // Notify interested parties
   AudioUnitParameter aup = {};
   aup.mAudioUnit = dstUnit;
   aup.mParameterID = kAUParameterListener_AnyParameter;
   aup.mScope = kAudioUnitScope_Global;
   aup.mElement = 0;
   AUParameterListenerNotify(NULL, NULL, &aup);

   return true;
}

unsigned AudioUnitEffect::GetChannelCount()
{
   return mNumChannels;
}

void AudioUnitEffect::SetChannelCount(unsigned numChannels)
{
   mNumChannels = numChannels;
}

OSStatus AudioUnitEffect::Render(AudioUnitRenderActionFlags *inActionFlags,
                                 const AudioTimeStamp *inTimeStamp,
                                 UInt32 inBusNumber,
                                 UInt32 inNumFrames,
                                 AudioBufferList *ioData)
{
   for (int i = 0; i < ioData->mNumberBuffers; i++)
      ioData->mBuffers[i].mData = mInputList[0].mBuffers[i].mData;

   return 0;
}

// static
OSStatus AudioUnitEffect::RenderCallback(void *inRefCon,
                                         AudioUnitRenderActionFlags *inActionFlags,
                                         const AudioTimeStamp *inTimeStamp,
                                         UInt32 inBusNumber,
                                         UInt32 inNumFrames,
                                         AudioBufferList *ioData)
{
   return ((AudioUnitEffect *) inRefCon)->Render(inActionFlags,
                                                 inTimeStamp,
                                                 inBusNumber,
                                                 inNumFrames,
                                                 ioData);
}

void AudioUnitEffect::EventListener(const AudioUnitEvent *inEvent,
                                    AudioUnitParameterValue inParameterValue)
{
   // Handle property changes
   if (inEvent->mEventType == kAudioUnitEvent_PropertyChange)
   {
      // Handle latency changes
      if (inEvent->mArgument.mProperty.mPropertyID == kAudioUnitProperty_Latency)
      {
         // Allow change to be used
         //mLatencyDone = false;
      }

      return;
   }

   // Only parameter changes at this point

   if (mMaster)
   {
      // We're a slave, so just set the parameter
      AudioUnitSetParameter(mUnit,
                            inEvent->mArgument.mParameter.mParameterID,
                            kAudioUnitScope_Global,
                            0,
                            inParameterValue,
                            0);
   }
   else
   {
      // We're the master, so propogate 
      for (size_t i = 0, cnt = mSlaves.size(); i < cnt; i++)
      {
         mSlaves[i]->EventListener(inEvent, inParameterValue);
      }
   }
}
                           
// static
void AudioUnitEffect::EventListenerCallback(void *inCallbackRefCon,
                                            void *inObject,
                                            const AudioUnitEvent *inEvent,
                                            UInt64 inEventHostTime,
                                            AudioUnitParameterValue inParameterValue)
{
   ((AudioUnitEffect *) inCallbackRefCon)->EventListener(inEvent,
                                                         inParameterValue);
}

void AudioUnitEffect::GetChannelCounts()
{
   Boolean isWritable = 0;
   UInt32  dataSize = 0;
   OSStatus result;

   // Does AU have channel info
   result = AudioUnitGetPropertyInfo(mUnit,
                                     kAudioUnitProperty_SupportedNumChannels,
                                     kAudioUnitScope_Global,
                                     0,
                                     &dataSize,
                                     &isWritable);
   if (result)
   {
      // None supplied.  Apparently all FX type units can do any number of INs
      // and OUTs as long as they are the same number.  In this case, we'll
      // just say stereo.
      //
      // We should probably check to make sure we're dealing with an FX type.
      mAudioIns = 2;
      mAudioOuts = 2;
      return;
   }

   ArrayOf<char> buffer{ dataSize };
   auto info = (AUChannelInfo *) buffer.get();

   // Retrieve the channel info
   result = AudioUnitGetProperty(mUnit,
                                 kAudioUnitProperty_SupportedNumChannels,
                                 kAudioUnitScope_Global,
                                 0,
                                 info,
                                 &dataSize);
   if (result)
   {
      // Oh well, not much we can do out this case
      mAudioIns = 2;
      mAudioOuts = 2;

      return;
   }

   // This is where it gets weird...not sure what is the best
   // way to do this really.  If we knew how many ins/outs we
   // really needed, we could make a better choice.

   bool haven2m = false;   // nothing -> mono
   bool haven2s = false;   // nothing -> stereo
   bool havem2n = false;   // mono -> nothing
   bool haves2n = false;   // stereo -> nothing
   bool havem2m = false;   // mono -> mono
   bool haves2s = false;   // stereo -> stereo
   bool havem2s = false;   // mono -> stereo
   bool haves2m = false;   // stereo -> mono

   mAudioIns = 2;
   mAudioOuts = 2;

   // Look only for exact channel constraints
   for (int i = 0; i < dataSize / sizeof(AUChannelInfo); i++)
   {
      AUChannelInfo *ci = &info[i];

      int ic = ci->inChannels;
      int oc = ci->outChannels;

      if (ic < 0 && oc >= 0)
      {
         ic = 2;
      }
      else if (ic >= 0 && oc < 0)
      {
         oc = 2;
      }
      else if (ic < 0 && oc < 0)
      {
         ic = 2;
         oc = 2;
      }

      if (ic == 2 && oc == 2)
      {
         haves2s = true;
      }
      else if (ic == 1 && oc == 1)
      {
         havem2m = true;
      }   
      else if (ic == 1 && oc == 2)
      {
         havem2s = true;
      }
      else if (ic == 2 && oc == 1)
      {
         haves2m = true;
      }
      else if (ic == 0 && oc == 2)
      {
         haven2s = true;
      }
      else if (ic == 0 && oc == 1)
      {
         haven2m = true;
      }
      else if (ic == 1 && oc == 0)
      {
         havem2n = true;
      }
      else if (ic == 2 && oc == 0)
      {
         haves2n = true;
      }
   }

   if (haves2s)
   {
      mAudioIns = 2;
      mAudioOuts = 2;
   }
   else if (havem2m)
   {
      mAudioIns = 1;
      mAudioOuts = 1;
   }
   else if (havem2s)
   {
      mAudioIns = 1;
      mAudioOuts = 2;
   }
   else if (haves2m)
   {
      mAudioIns = 2;
      mAudioOuts = 1;
   }
   else if (haven2m)
   {
      mAudioIns = 0;
      mAudioOuts = 1;
   }
   else if (haven2s)
   {
      mAudioIns = 0;
      mAudioOuts = 2;
   }
   else if (haves2n)
   {
      mAudioIns = 2;
      mAudioOuts = 0;
   }
   else if (havem2n)
   {
      mAudioIns = 1;
      mAudioOuts = 0;
   }

   return;
}

#endif
