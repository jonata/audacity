/**********************************************************************

  Audacity: A Digital Audio Editor

  HelpText.h

  James Crook

**********************************************************************/

#ifndef __AUDACITY_HELP_TEXT__
#define __AUDACITY_HELP_TEXT__

class wxString;

wxString HelpText( const wxString & Key );
wxString TitleText( const wxString & Key );

extern const wxString VerCheckArgs();
extern const wxString VerCheckUrl();
extern const wxString VerCheckHtml();

#endif
