////////////////////////////////////////////////////////////////////////////
//	Author		: Maxim Kornienko
//	Description : report - menu
////////////////////////////////////////////////////////////////////////////

#include "dataReport.h"
#include "frontend/metatree/metatreeWnd.h"

bool CMetaObjectReportValue::PrepareContextMenu(wxMenu *defultMenu)
{
	wxMenuItem *m_menuItem = NULL;
	m_menuItem = defultMenu->Append(ID_METATREE_OPEN_MODULE, _("open module"));
	m_menuItem->SetBitmap(wxGetImageBMPFromResource(IDB_EDIT_MODULE));
	m_menuItem = defultMenu->Append(ID_METATREE_OPEN_MANAGER, _("open manager"));
	m_menuItem->SetBitmap(wxGetImageBMPFromResource(IDB_EDIT_MODULE));
	defultMenu->AppendSeparator();
	return false;
}

void CMetaObjectReportValue::ProcessCommand(unsigned int id)
{
	IMetadataTree *metaTree = m_metaData->GetMetaTree();
	wxASSERT(metaTree);

	if (id == ID_METATREE_OPEN_MODULE)
		metaTree->OpenFormMDI(m_moduleObject);
	else if (id == ID_METATREE_OPEN_MANAGER)
		metaTree->OpenFormMDI(m_moduleManager);
}