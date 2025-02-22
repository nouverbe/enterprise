////////////////////////////////////////////////////////////////////////////
//	Author		: Maxim Kornienko
//	Description : dataProcessor - metadata
////////////////////////////////////////////////////////////////////////////

#include "dataProcessor.h"
#include "metadata/metadata.h"

#define objectModule wxT("objectModule")
#define managerModule wxT("managerModule")

wxIMPLEMENT_DYNAMIC_CLASS(CMetaObjectDataProcessorValue, IMetaObjectValue)
wxIMPLEMENT_DYNAMIC_CLASS(CMetaObjectDataProcessorExternalValue, CMetaObjectDataProcessorValue)

//********************************************************************************************
//*                                      metadata                                            *
//********************************************************************************************

CMetaObjectDataProcessorValue::CMetaObjectDataProcessorValue() : IMetaObjectValue(),
m_defaultFormObject(wxNOT_FOUND), m_objMode(METAOBJECT_NORMAL)
{
	PropertyContainer *categoryForm = IObjectBase::CreatePropertyContainer("DefaultForms");
	categoryForm->AddProperty("default_object", PropertyType::PT_OPTION, &CMetaObjectDataProcessorValue::GetFormObject);
	m_category->AddCategory(categoryForm);

	//create module
	m_moduleObject = new CMetaModuleObject(objectModule);
	m_moduleObject->SetClsid(g_metaModuleCLSID);

	//set child/parent
	m_moduleObject->SetParent(this);
	AddChild(m_moduleObject);

	m_moduleManager = new CMetaManagerModuleObject(managerModule);
	m_moduleManager->SetClsid(g_metaManagerCLSID);

	//set child/parent
	m_moduleManager->SetParent(this);
	AddChild(m_moduleManager);
}


CMetaObjectDataProcessorValue::CMetaObjectDataProcessorValue(int objMode) : IMetaObjectValue(),
m_defaultFormObject(wxNOT_FOUND), m_objMode(objMode)
{
	PropertyContainer *categoryForm = IObjectBase::CreatePropertyContainer("DefaultForms");
	categoryForm->AddProperty("default_object", PropertyType::PT_OPTION, &CMetaObjectDataProcessorValue::GetFormObject);
	m_category->AddCategory(categoryForm);

	//create module
	m_moduleObject = new CMetaModuleObject(objectModule);
	m_moduleObject->SetClsid(g_metaModuleCLSID);

	//set child/parent
	m_moduleObject->SetParent(this);
	AddChild(m_moduleObject);

	m_moduleManager = new CMetaManagerModuleObject(managerModule);
	m_moduleManager->SetClsid(g_metaManagerCLSID);

	//set child/parent
	m_moduleManager->SetParent(this);
	AddChild(m_moduleManager);
}

CMetaObjectDataProcessorValue::~CMetaObjectDataProcessorValue()
{
	wxDELETE(m_moduleObject);
	wxDELETE(m_moduleManager);
}

CMetaFormObject *CMetaObjectDataProcessorValue::GetDefaultFormByID(form_identifier_t id)
{
	if (id == eFormDataProcessor
		&& m_defaultFormObject != wxNOT_FOUND) {
		for (auto obj : GetObjectForms()) {
			if (m_defaultFormObject == obj->GetMetaID()) {
				return obj;
			}
		}
	}

	return NULL;
}

IDataObjectSource *CMetaObjectDataProcessorValue::CreateObjectData(IMetaFormObject *metaObject)
{
	switch (metaObject->GetTypeForm())
	{
	case eFormDataProcessor: return CreateObjectValue(); break;
	}

	return NULL;
}

#include "appData.h"

IDataObjectValue *CMetaObjectDataProcessorValue::CreateObjectValue()
{
	IModuleManager *moduleManager = m_metaData->GetModuleManager();
	wxASSERT(moduleManager);

	IDataObjectValue *pDataRef = NULL;

	if (appData->DesignerMode()) {
		if (m_objMode == METAOBJECT_NORMAL) {
			if (!moduleManager->FindCompileModule(m_moduleObject, pDataRef))
				return new CObjectDataProcessorValue(this);
		}
		else {
			return moduleManager->GetObjectValue();
		}
	}
	else {
		if (m_objMode == METAOBJECT_NORMAL) {
			pDataRef = new CObjectDataProcessorValue(this);
		}
		else {
			return moduleManager->GetObjectValue();
		}
	}

	return pDataRef;
}

CValueForm *CMetaObjectDataProcessorValue::CreateObjectValue(IMetaFormObject *metaForm)
{
	return metaForm->GenerateFormAndRun(
		NULL, CreateObjectData(metaForm)
	);
}

#include "frontend/visualView/controls/form.h"
#include "utils/stringUtils.h"

CValueForm *CMetaObjectDataProcessorValue::GetObjectForm(const wxString &formName, IValueFrame *ownerControl, const Guid &formGuid)
{
	CMetaFormObject* defList = NULL;

	if (!formName.IsEmpty()) {
		for (auto metaForm : GetObjectForms()) {
			if (CMetaObjectDataProcessorValue::eFormDataProcessor == metaForm->GetTypeForm()
				&& StringUtils::CompareString(formName, metaForm->GetName())) {
				return defList->GenerateFormAndRun(
					ownerControl, CreateObjectValue()
				);
			}
		}
		wxASSERT(defList);
	}
	else {
		defList = GetDefaultFormByID(CMetaObjectDataProcessorValue::eFormDataProcessor);
		if (defList) {
			return defList->GenerateFormAndRun(
				ownerControl, CreateObjectValue(), formGuid
			);
		}
	}

	if (defList == NULL) {
		IDataObjectValue *objectData = CreateObjectValue();
		CValueForm *valueForm = new CValueForm;
		valueForm->InitializeForm(ownerControl, NULL,
			objectData, formGuid
		);
		valueForm->ReadProperty();
		valueForm->BuildForm(CMetaObjectDataProcessorValue::eFormDataProcessor);
		return valueForm;
	}

	return defList->GenerateFormAndRun();
}

OptionList *CMetaObjectDataProcessorValue::GetFormObject(Property *)
{
	OptionList *optlist = new OptionList;
	optlist->AddOption("<not selected>", wxNOT_FOUND);

	for (auto formObject : GetObjectForms()) {
		if (eFormDataProcessor == formObject->GetTypeForm()) {
			optlist->AddOption(formObject->GetName(), formObject->GetMetaID());
		}
	}

	return optlist;
}

//***************************************************************************
//*                       Save & load metadata                              *
//***************************************************************************

bool CMetaObjectDataProcessorValue::LoadData(CMemoryReader &dataReader)
{
	//Load object module
	m_moduleObject->LoadMeta(dataReader);
	m_moduleManager->LoadMeta(dataReader);

	//Load default form 
	m_defaultFormObject = dataReader.r_u32();

	return IMetaObjectValue::LoadData(dataReader);
}

bool CMetaObjectDataProcessorValue::SaveData(CMemoryWriter &dataWritter)
{
	//Save object module
	m_moduleObject->SaveMeta(dataWritter);
	m_moduleManager->SaveMeta(dataWritter);

	//Save default form 
	dataWritter.w_u32(m_defaultFormObject);

	return IMetaObjectValue::SaveData(dataWritter);
}

//***********************************************************************
//*                           read & save events                        *
//***********************************************************************

#include "appData.h"

bool CMetaObjectDataProcessorValue::OnCreateMetaObject(IMetadata *metaData)
{
	if (!IMetaObjectValue::OnCreateMetaObject(metaData))
		return false;

	return (m_objMode == METAOBJECT_NORMAL ? m_moduleManager->OnCreateMetaObject(metaData) : true) &&
		m_moduleObject->OnCreateMetaObject(metaData);
}

bool CMetaObjectDataProcessorValue::OnLoadMetaObject(IMetadata *metaData)
{
	IModuleManager *moduleManager = m_metaData->GetModuleManager();
	wxASSERT(moduleManager);

	if (m_objMode == METAOBJECT_NORMAL) {

		if (!m_moduleManager->OnLoadMetaObject(metaData))
			return false;

		if (!m_moduleObject->OnLoadMetaObject(metaData))
			return false;
	}
	else {

		if (!m_moduleObject->OnLoadMetaObject(metaData))
			return false;
	}

	return IMetaObjectValue::OnLoadMetaObject(metaData);
}

bool CMetaObjectDataProcessorValue::OnSaveMetaObject()
{
	if (m_objMode == METAOBJECT_NORMAL) {
		if (!m_moduleManager->OnSaveMetaObject())
			return false;
	}

	if (!m_moduleObject->OnSaveMetaObject())
		return false;

	return IMetaObjectValue::OnSaveMetaObject();
}

bool CMetaObjectDataProcessorValue::OnDeleteMetaObject()
{
	IModuleManager *moduleManager = m_metaData->GetModuleManager();
	wxASSERT(moduleManager);

	if (m_objMode == METAOBJECT_NORMAL) {
		if (!m_moduleManager->OnDeleteMetaObject())
			return false;
	}

	if (!m_moduleObject->OnDeleteMetaObject())
		return false;

	return IMetaObjectValue::OnDeleteMetaObject();
}

bool CMetaObjectDataProcessorValue::OnReloadMetaObject()
{
	IModuleManager *moduleManager = m_metaData->GetModuleManager();
	wxASSERT(moduleManager);

	if (appData->DesignerMode()) {
		CObjectDataProcessorValue *pDataRef = NULL;
		if (!moduleManager->FindCompileModule(m_moduleObject, pDataRef)) {
			return false;
		}
		return pDataRef->InitializeObject();
	}

	return true;
}

bool CMetaObjectDataProcessorValue::OnRunMetaObject(int flags)
{
	IModuleManager *moduleManager = m_metaData->GetModuleManager();
	wxASSERT(moduleManager);

	if (m_objMode == METAOBJECT_NORMAL) {
		if (!m_moduleManager->OnRunMetaObject(flags)) {
			return false;
		}
	}

	if (!m_moduleObject->OnRunMetaObject(flags)) {
		return false;
	}

	if (appData->DesignerMode()) {
		if (moduleManager->AddCompileModule(m_moduleObject, CreateObjectValue())) {
			return IMetaObjectValue::OnRunMetaObject(flags);
		}
		return false;
	}
	else {
		return IMetaObjectValue::OnRunMetaObject(flags);
	}
}

bool CMetaObjectDataProcessorValue::OnCloseMetaObject()
{
	IModuleManager *moduleManager = m_metaData->GetModuleManager();
	wxASSERT(moduleManager);

	if (m_objMode == METAOBJECT_NORMAL) {
		if (!m_moduleManager->OnCloseMetaObject())
			return false;
	}

	if (!m_moduleObject->OnCloseMetaObject()) {
		return false;
	}

	if (appData->DesignerMode()) {
		if (moduleManager->RemoveCompileModule(m_moduleObject)) {
			return IMetaObjectValue::OnCloseMetaObject();
		}
		return false;
	}

	return IMetaObjectValue::OnCloseMetaObject();
}

//***********************************************************************
//*                             form events                             *
//***********************************************************************

void CMetaObjectDataProcessorValue::OnCreateMetaForm(IMetaFormObject *metaForm)
{
	if (metaForm->GetTypeForm() == CMetaObjectDataProcessorValue::eFormDataProcessor
		&& m_defaultFormObject == wxNOT_FOUND)
	{
		m_defaultFormObject = metaForm->GetMetaID();
	}
}

void CMetaObjectDataProcessorValue::OnRemoveMetaForm(IMetaFormObject *metaForm)
{
	if (metaForm->GetTypeForm() == CMetaObjectDataProcessorValue::eFormDataProcessor
		&& m_defaultFormObject == metaForm->GetMetaID())
	{
		m_defaultFormObject = wxNOT_FOUND;
	}
}

//***********************************************************************
//*                           read & save property                      *
//***********************************************************************

void CMetaObjectDataProcessorValue::ReadProperty()
{
	IMetaObjectValue::ReadProperty();

	IObjectBase::SetPropertyValue("default_object", m_defaultFormObject);
}

void CMetaObjectDataProcessorValue::SaveProperty()
{
	IMetaObjectValue::SaveProperty();

	IObjectBase::GetPropertyValue("default_object", m_defaultFormObject);
}

//***********************************************************************
//*                       Register in runtime                           *
//***********************************************************************

METADATA_REGISTER(CMetaObjectDataProcessorValue, "metaDataDataProcessor", g_metaDataProcessorCLSID);
METADATA_REGISTER(CMetaObjectDataProcessorExternalValue, "metaExternalDataDataProcessor", g_metaExternalDataProcessorCLSID);