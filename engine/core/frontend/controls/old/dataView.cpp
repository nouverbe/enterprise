/////////////////////////////////////////////////////////////////////////////
// Name:        dataView.cpp
// Purpose:     CDataViewCtrl generic implementation
// Author:      Robert Roebling
// Modified by: Francesco Montorsi, Guru Kathiresan, Bo Yang
// Copyright:   (c) 1998 Robert Roebling
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx.h".
#include <wx/wxprec.h>

#if wxUSE_DATAVIEWCTRL

#include "dataView.h"

#ifdef wxHAS_GENERIC_DATAVIEWCTRL

#ifndef WX_PRECOMP
#ifdef __WXMSW__
#include "wx/app.h"          // GetRegisteredClassName()
#include "wx/msw/private.h"
#include "wx/msw/wrapwin.h"
#include "wx/msw/wrapcctl.h" // include <commctrl.h> "properly"
#endif
#include "wx/sizer.h"
#include "wx/log.h"
#include "wx/dcclient.h"
#include "wx/timer.h"
#include "wx/settings.h"
#include "wx/msgdlg.h"
#include "wx/dcscreen.h"
#include "wx/frame.h"
#include "wx/vector.h"
#endif

#include "wx/stockitem.h"
#include "wx/popupwin.h"
#include "wx/renderer.h"
#include "wx/dcbuffer.h"
#include "wx/icon.h"
#include "wx/itemattr.h"
#include "wx/list.h"
#include "wx/listimpl.cpp"
#include "wx/imaglist.h"
#include "wx/headerctrl.h"
#include "wx/dnd.h"
#include "wx/selstore.h"
#include "wx/stopwatch.h"
#include "wx/weakref.h"
#include "wx/generic/private/markuptext.h"
#include "wx/generic/private/rowheightcache.h"
#include "wx/generic/private/widthcalc.h"
#if wxUSE_ACCESSIBILITY
#include "wx/private/markupparser.h"
#endif // wxUSE_ACCESSIBILITY

//-----------------------------------------------------------------------------
// classes
//-----------------------------------------------------------------------------

class CDataViewColumn;
class CDataViewHeaderWindow;
class CDataViewCtrl;

//-----------------------------------------------------------------------------
// constants
//-----------------------------------------------------------------------------

// the cell padding on the left/right
static const int PADDING_RIGHTLEFT = 3;

namespace
{

	// Flags for Walker() function defined below.
	enum WalkFlags
	{
		Walk_All,               // Visit all items.
		Walk_ExpandedOnly       // Visit only expanded items.
	};

	// The column is either the index of the column to be used for sorting or one
	// of the special values in this enum:
	enum
	{
		// Don't sort at all.
		SortColumn_None = -2,

		// Sort using the model default sort order.
		SortColumn_Default = -1
	};

	// A class storing the definition of sort order used, as a column index and
	// sort direction by this column.
	//
	// Notice that the sort order may be invalid, meaning that items shouldn't be
	// sorted.
	class SortOrder
	{
	public:
		explicit SortOrder(int column = SortColumn_None, bool ascending = true)
			: m_column(column),
			m_ascending(ascending)
		{
		}

		// Default copy ctor, assignment operator and dtor are all OK.

		bool IsNone() const { return m_column == SortColumn_None; }

		bool UsesColumn() const { return m_column >= 0; }

		int GetColumn() const { return m_column; }
		bool IsAscending() const { return m_ascending; }

		bool operator==(const SortOrder& other) const
		{
			return m_column == other.m_column && m_ascending == other.m_ascending;
		}

		bool operator!=(const SortOrder& other) const
		{
			return !(*this == other);
		}

	private:
		int m_column;
		bool m_ascending;
	};

	// ----------------------------------------------------------------------------
	// helper functions
	// ----------------------------------------------------------------------------

	// Return the expander column or, if it is not set, the first column and also
	// set it as the expander one for the future.
	CDataViewColumn* GetExpanderColumnOrFirstOne(CDataViewCtrl* dataview)
	{
		CDataViewColumn* expander = dataview->GetExpanderColumn();
		if (!expander)
		{
			// TODO-RTL: last column for RTL support
			expander = dataview->GetColumnAt(0);
			dataview->SetExpanderColumn(expander);
		}

		return expander;
	}

	wxTextCtrl *CreateEditorTextCtrl(wxWindow *parent, const wxRect& labelRect, const wxString& value)
	{
		wxTextCtrl* ctrl = new wxTextCtrl(parent, wxID_ANY, value,
			labelRect.GetPosition(),
			labelRect.GetSize(),
			wxTE_PROCESS_ENTER);

		// Adjust size of wxTextCtrl editor to fit text, even if it means being
		// wider than the corresponding column (this is how Explorer behaves).
		const int fitting = ctrl->GetSizeFromTextSize(ctrl->GetTextExtent(ctrl->GetValue())).x;
		const int current = ctrl->GetSize().x;
		const int maxwidth = ctrl->GetParent()->GetSize().x - ctrl->GetPosition().x;

		// Adjust size so that it fits all content. Don't change anything if the
		// allocated space is already larger than needed and don't extend wxDVC's
		// boundaries.
		int width = wxMin(wxMax(current, fitting), maxwidth);

		if (width != current)
			ctrl->SetSize(wxSize(width, -1));

		// select the text in the control an place the cursor at the end
		ctrl->SetInsertionPointEnd();
		ctrl->SelectAll();

		return ctrl;
	}

} // anonymous namespace

//-----------------------------------------------------------------------------
// CDataViewColumn
//-----------------------------------------------------------------------------

void CDataViewColumn::Init(int width, wxAlignment align, int flags)
{
	m_width =
		m_manuallySetWidth = width;
	m_minWidth = 0;
	m_align = align;
	m_flags = flags;
	m_sort = false;
	m_sortAscending = true;
}

int CDataViewColumn::DoGetEffectiveWidth(int width) const
{
	switch (width)
	{
	case wxCOL_WIDTH_DEFAULT:
		return wxWindow::FromDIP(wxDVC_DEFAULT_WIDTH, m_owner);

	case wxCOL_WIDTH_AUTOSIZE:
		wxCHECK_MSG(m_owner, wxDVC_DEFAULT_WIDTH, "no owner control");
		return m_owner->GetBestColumnWidth(m_owner->GetColumnIndex(this));

	default:
		return width;
	}
}

int CDataViewColumn::GetWidth() const
{
	return DoGetEffectiveWidth(m_width);
}

void CDataViewColumn::WXOnResize(int width)
{
	m_width =
		m_manuallySetWidth = width;

	m_owner->OnColumnResized();
}

int CDataViewColumn::WXGetSpecifiedWidth() const
{
	// Note that we need to return valid value even if no width was initially
	// specified, as otherwise the last column created without any explicit
	// width could be reduced to nothing by UpdateColumnSizes() when the
	// control is shrunk.
	return DoGetEffectiveWidth(m_manuallySetWidth);
}

void CDataViewColumn::UpdateDisplay()
{
	if (m_owner)
	{
		int idx = m_owner->GetColumnIndex(this);
		m_owner->OnColumnChange(idx);
	}
}

void CDataViewColumn::UpdateWidth()
{
	if (m_owner)
	{
		int idx = m_owner->GetColumnIndex(this);
		m_owner->OnColumnWidthChange(idx);
	}
}

void CDataViewColumn::UnsetAsSortKey()
{
	m_sort = false;

	if (m_owner)
		m_owner->DontUseColumnForSorting(m_owner->GetColumnIndex(this));

	UpdateDisplay();
}

void CDataViewColumn::SetSortOrder(bool ascending)
{
	if (!m_owner)
		return;

	const int idx = m_owner->GetColumnIndex(this);

	// If this column isn't sorted already, mark it as sorted
	if (!m_sort)
	{
		wxASSERT(!m_owner->IsColumnSorted(idx));

		// Now set this one as the new sort column.
		m_owner->UseColumnForSorting(idx);
		m_sort = true;
	}

	m_sortAscending = ascending;

	// Call this directly instead of using UpdateDisplay() as we already have
	// the column index, no need to look it up again.
	m_owner->OnColumnChange(idx);
}

//-----------------------------------------------------------------------------
// CDataViewHeaderWindow
//-----------------------------------------------------------------------------

class CDataViewHeaderWindow : public wxHeaderCtrl
{
public:
	CDataViewHeaderWindow(CDataViewCtrl *parent)
		: wxHeaderCtrl(parent, wxID_ANY,
			wxDefaultPosition, wxDefaultSize,
			wxHD_DEFAULT_STYLE | wxHD_BITMAP_ON_RIGHT)
	{
	}

	CDataViewCtrl *GetOwner() const
	{
		return static_cast<CDataViewCtrl *>(GetParent());
	}

	// Add/Remove additional column to sorting columns
	void ToggleSortByColumn(int column)
	{
		CDataViewCtrl * const owner = GetOwner();

		if (!owner->IsMultiColumnSortAllowed())
			return;

		CDataViewColumn * const col = owner->GetColumn(column);
		if (!col->IsSortable())
			return;

		if (owner->IsColumnSorted(column))
		{
			col->UnsetAsSortKey();
			SendEvent(wxEVT_DATAVIEW_COLUMN_SORTED, column);
		}
		else // Do start sortign by it.
		{
			col->SetSortOrder(true);
			SendEvent(wxEVT_DATAVIEW_COLUMN_SORTED, column);
		}
	}

#if wxUSE_ACCESSIBILITY
	virtual wxAccessible* CreateAccessible() wxOVERRIDE
	{
		// Under MSW wxHeadrCtrl is a native control
		// so we just need to pass all requests
		// to the accessibility framework.
		return new wxAccessible(this);
	}
#endif // wxUSE_ACCESSIBILITY

protected:
	// implement/override wxHeaderCtrl functions by forwarding them to the main
	// control
	virtual const wxHeaderColumn& GetColumn(unsigned int idx) const wxOVERRIDE
	{
		return *(GetOwner()->GetColumn(idx));
	}

	virtual bool UpdateColumnWidthToFit(unsigned int idx, int widthTitle) wxOVERRIDE
	{
		CDataViewCtrl * const owner = GetOwner();

		int widthContents = owner->GetBestColumnWidth(idx);
		owner->GetColumn(idx)->SetWidth(wxMax(widthTitle, widthContents));
		owner->OnColumnChange(idx);

		return true;
	}

private:
	void FinishEditing();

	bool SendEvent(wxEventType type, unsigned int n)
	{
		CDataViewCtrl * const owner = GetOwner();
		CDataViewEvent event(type, owner, owner->GetColumn(n));

		// for events created by CDataViewHeaderWindow the
		// row / value fields are not valid
		return owner->ProcessWindowEvent(event);
	}

	void OnClick(wxHeaderCtrlEvent& event)
	{
		FinishEditing();

		const unsigned idx = event.GetColumn();

		if (SendEvent(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK, idx))
			return;

		// default handling for the column click is to sort by this column or
		// toggle its sort order
		CDataViewCtrl * const owner = GetOwner();
		CDataViewColumn * const col = owner->GetColumn(idx);
		if (!col->IsSortable())
		{
			// no default handling for non-sortable columns
			event.Skip();
			return;
		}

		if (col->IsSortKey())
		{
			// already using this column for sorting, just change the order
			col->ToggleSortOrder();
		}
		else // not using this column for sorting yet
		{
			// We will sort by this column only now, so reset all the
			// previously used ones.
			owner->ResetAllSortColumns();

			// Sort the column
			col->SetSortOrder(true);
		}

		CDataViewModel * const model = owner->GetModel();
		if (model)
			model->Resort();

		owner->OnColumnChange(idx);

		SendEvent(wxEVT_DATAVIEW_COLUMN_SORTED, idx);
	}

	void OnRClick(wxHeaderCtrlEvent& event)
	{
		// Event wasn't processed somewhere, use default behaviour
		if (!SendEvent(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
			event.GetColumn()))
		{
			event.Skip();
			ToggleSortByColumn(event.GetColumn());
		}
	}

	void OnResize(wxHeaderCtrlEvent& event)
	{
		FinishEditing();

		CDataViewCtrl * const owner = GetOwner();

		const unsigned col = event.GetColumn();
		owner->GetColumn(col)->WXOnResize(event.GetWidth());
	}

	void OnEndReorder(wxHeaderCtrlEvent& event)
	{
		FinishEditing();

		CDataViewCtrl * const owner = GetOwner();
		owner->ColumnMoved(owner->GetColumn(event.GetColumn()),
			event.GetNewOrder());
	}

	wxDECLARE_EVENT_TABLE();
	wxDECLARE_NO_COPY_CLASS(CDataViewHeaderWindow);
};

wxBEGIN_EVENT_TABLE(CDataViewHeaderWindow, wxHeaderCtrl)
EVT_HEADER_CLICK(wxID_ANY, CDataViewHeaderWindow::OnClick)
EVT_HEADER_RIGHT_CLICK(wxID_ANY, CDataViewHeaderWindow::OnRClick)

EVT_HEADER_RESIZING(wxID_ANY, CDataViewHeaderWindow::OnResize)
EVT_HEADER_END_RESIZE(wxID_ANY, CDataViewHeaderWindow::OnResize)

EVT_HEADER_END_REORDER(wxID_ANY, CDataViewHeaderWindow::OnEndReorder)
wxEND_EVENT_TABLE()

//-----------------------------------------------------------------------------
// CDataViewRenameTimer
//-----------------------------------------------------------------------------

class CDataViewRenameTimer : public wxTimer
{
private:
	CDataViewMainWindow *m_owner;

public:
	CDataViewRenameTimer(CDataViewMainWindow *owner);
	void Notify() wxOVERRIDE;
};

//-----------------------------------------------------------------------------
// CDataViewTreeNode
//-----------------------------------------------------------------------------

class CDataViewMainWindow;
class CDataViewTreeNode;

typedef wxVector<CDataViewTreeNode*> CDataViewTreeNodes;

// Note: this class is not used at all for virtual list models, so all code
// using it, i.e. any functions taking or returning objects of this type,
// including CDataViewMainWindow::m_root, can only be called after checking
// that we're using a non-"virtual list" model.

class CDataViewTreeNode
{
public:
	CDataViewTreeNode(CDataViewTreeNode *parent, const wxDataViewItem& item)
		: m_parent(parent),
		m_item(item),
		m_branchData(NULL)
	{
	}

	~CDataViewTreeNode()
	{
		if (m_branchData)
		{
			CDataViewTreeNodes& nodes = m_branchData->children;
			for (CDataViewTreeNodes::iterator i = nodes.begin();
				i != nodes.end();
				++i)
			{
				delete *i;
			}

			delete m_branchData;
		}
	}

	static CDataViewTreeNode* CreateRootNode()
	{
		CDataViewTreeNode *n = new CDataViewTreeNode(NULL, wxDataViewItem());
		n->m_branchData = new BranchNodeData;
		n->m_branchData->open = true;
		return n;
	}

	CDataViewTreeNode * GetParent() const { return m_parent; }

	const CDataViewTreeNodes& GetChildNodes() const
	{
		return m_branchData->children;
	}

	void InsertChild(CDataViewMainWindow* window,
		CDataViewTreeNode *node, unsigned index);

	void RemoveChild(unsigned index)
	{
		wxCHECK_RET(m_branchData != NULL, "leaf node doesn't have children");
		m_branchData->RemoveChild(index);
	}

	// returns position of child node for given item in children list or wxNOT_FOUND
	int FindChildByItem(const wxDataViewItem& item) const
	{
		if (!m_branchData)
			return wxNOT_FOUND;

		const CDataViewTreeNodes& nodes = m_branchData->children;
		const int len = nodes.size();
		for (int i = 0; i < len; i++)
		{
			if (nodes[i]->m_item == item)
				return i;
		}
		return wxNOT_FOUND;
	}

	const wxDataViewItem & GetItem() const { return m_item; }
	void SetItem(const wxDataViewItem & item) { m_item = item; }

	int GetIndentLevel() const
	{
		int ret = 0;
		const CDataViewTreeNode * node = this;
		while (node->GetParent()->GetParent() != NULL)
		{
			node = node->GetParent();
			ret++;
		}
		return ret;
	}

	bool IsOpen() const
	{
		return m_branchData && m_branchData->open;
	}

	void ToggleOpen(CDataViewMainWindow* window)
	{
		// We do not allow the (invisible) root node to be collapsed because
		// there is no way to expand it again.
		if (!m_parent)
			return;

		wxCHECK_RET(m_branchData != NULL, "can't open leaf node");

		int sum = 0;

		const CDataViewTreeNodes& nodes = m_branchData->children;
		const int len = nodes.size();
		for (int i = 0; i < len; i++)
			sum += 1 + nodes[i]->GetSubTreeCount();

		if (m_branchData->open)
		{
			ChangeSubTreeCount(-sum);
			m_branchData->open = !m_branchData->open;
		}
		else
		{
			m_branchData->open = !m_branchData->open;
			ChangeSubTreeCount(+sum);
			// Sort the children if needed
			Resort(window);
		}
	}

	// "HasChildren" property corresponds to model's IsContainer(). Note that it may be true
	// even if GetChildNodes() is empty; see below.
	bool HasChildren() const
	{
		return m_branchData != NULL;
	}

	void SetHasChildren(bool has)
	{
		// The invisible root item always has children, so ignore any attempts
		// to change this.
		if (!m_parent)
			return;

		if (!has)
		{
			wxDELETE(m_branchData);
		}
		else if (m_branchData == NULL)
		{
			m_branchData = new BranchNodeData;
		}
	}

	int GetSubTreeCount() const
	{
		return m_branchData ? m_branchData->subTreeCount : 0;
	}

	void ChangeSubTreeCount(int num)
	{
		wxASSERT(m_branchData != NULL);

		if (!m_branchData->open)
			return;

		m_branchData->subTreeCount += num;
		wxASSERT(m_branchData->subTreeCount >= 0);

		if (m_parent)
			m_parent->ChangeSubTreeCount(num);
	}

	void Resort(CDataViewMainWindow* window);

	// Should be called after changing the item value to update its position in
	// the control if necessary.
	void PutInSortOrder(CDataViewMainWindow* window)
	{
		if (m_parent)
			m_parent->PutChildInSortOrder(window, this);
	}

private:
	// Called by the child after it has been updated to put it in the right
	// place among its siblings, depending on the sort order.
	//
	// The argument must be non-null, but is passed as a pointer as it's
	// inserted into m_branchData->children.
	void PutChildInSortOrder(CDataViewMainWindow* window,
		CDataViewTreeNode* childNode);

	CDataViewTreeNode  *m_parent;

	// Corresponding model item.
	wxDataViewItem       m_item;

	// Data specific to non-leaf (branch, inner) nodes. They are kept in a
	// separate struct in order to conserve memory.
	struct BranchNodeData
	{
		BranchNodeData()
			: open(false),
			subTreeCount(0)
		{
		}

		void InsertChild(CDataViewTreeNode* node, unsigned index)
		{
			children.insert(children.begin() + index, node);
		}

		void RemoveChild(unsigned index)
		{
			children.erase(children.begin() + index);
		}

		// Child nodes. Note that this may be empty even if m_hasChildren in
		// case this branch of the tree wasn't expanded and realized yet.
		CDataViewTreeNodes  children;

		// Order in which children are sorted (possibly none).
		SortOrder            sortOrder;

		// Is the branch node currently open (expanded)?
		bool                 open;

		// Total count of expanded (i.e. visible with the help of some
		// scrolling) items in the subtree, but excluding this node. I.e. it is
		// 0 for leaves and is the number of rows the subtree occupies for
		// branch nodes.
		int                  subTreeCount;
	};

	BranchNodeData *m_branchData;
};



//-----------------------------------------------------------------------------
// CDataViewMainWindow
//-----------------------------------------------------------------------------

class CDataViewMainWindow : public wxWindow
{
public:
	CDataViewMainWindow(CDataViewCtrl *parent,
		wxWindowID id,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		const wxString &name = wxT("CDataViewCtrlmainwindow"));
	virtual ~CDataViewMainWindow();

	bool IsList() const { return GetModel()->IsListModel(); }
	bool IsVirtualList() const { return m_root == NULL; }

	// notifications from CDataViewModel
	bool ItemAdded(const wxDataViewItem &parent, const wxDataViewItem &item);
	bool ItemDeleted(const wxDataViewItem &parent, const wxDataViewItem &item);
	bool ItemChanged(const wxDataViewItem &item)
	{
		return DoItemChanged(item, wxNOT_FOUND);
	}
	bool ValueChanged(const wxDataViewItem &item, unsigned int model_column);
	bool Cleared();
	void Resort()
	{
		ClearRowHeightCache();

		if (!IsVirtualList())
		{
			m_root->Resort(this);
		}
		UpdateDisplay();
	}
	void ClearRowHeightCache()
	{
		if (m_rowHeightCache)
			m_rowHeightCache->Clear();
	}

	SortOrder GetSortOrder() const
	{
		CDataViewColumn* const col = GetOwner()->GetSortingColumn();
		if (col)
		{
			return SortOrder(col->GetModelColumn(),
				col->IsSortOrderAscending());
		}
		else
		{
			if (GetModel()->HasDefaultCompare())
				return SortOrder(SortColumn_Default);
			else
				return SortOrder();
		}
	}

	void SetOwner(CDataViewCtrl* owner) { m_owner = owner; }
	CDataViewCtrl *GetOwner() { return m_owner; }
	const CDataViewCtrl *GetOwner() const { return m_owner; }

	CDataViewModel* GetModel() { return GetOwner()->GetModel(); }
	const CDataViewModel* GetModel() const { return GetOwner()->GetModel(); }

#if wxUSE_DRAG_AND_DROP
	wxBitmap CreateItemBitmap(unsigned int row, int &indent);
#endif // wxUSE_DRAG_AND_DROP
	void OnPaint(wxPaintEvent &event);
	void OnCharHook(wxKeyEvent &event);
	void OnChar(wxKeyEvent &event);
	void OnLeftKey(wxKeyEvent& event);
	void OnRightKey(wxKeyEvent& event);
	void OnMouse(wxMouseEvent &event);
	void OnSetFocus(wxFocusEvent &event);
	void OnKillFocus(wxFocusEvent &event);

	// Go to the specified row, i.e. make it current and change or extend the
	// selection extended depending on the modifier keys flags in the keyboard
	// state.
	//
	// The row must be valid.
	void GoToRow(const wxKeyboardState& state, unsigned int row);

	// Go to the item at position delta rows away (delta may be positive or
	// negative) from the current row.
	//
	// If adding delta would result in an invalid item, it's clamped to the
	// valid items range.
	void GoToRelativeRow(const wxKeyboardState& kbdState, int delta);

	void UpdateDisplay();
	void RecalculateDisplay();
	void OnInternalIdle() wxOVERRIDE;

	void OnRenameTimer();

	void ScrollWindow(int dx, int dy, const wxRect *rect = NULL) wxOVERRIDE;
	void ScrollTo(int rows, int column);

	unsigned GetCurrentRow() const { return m_currentRow; }
	bool HasCurrentRow() { return m_currentRow != (unsigned int)-1; }
	void ChangeCurrentRow(unsigned int row);
	bool TryAdvanceCurrentColumn(CDataViewTreeNode *node, wxKeyEvent& event, bool forward);

	CDataViewColumn *GetCurrentColumn() const { return m_currentCol; }
	void ClearCurrentColumn() { m_currentCol = NULL; }

	bool IsSingleSel() const { return !GetParent()->HasFlag(wxDV_MULTIPLE); }
	bool IsEmpty() { return GetRowCount() == 0; }

	int GetCountPerPage() const;
	int GetEndOfLastCol() const;
	unsigned int GetFirstVisibleRow() const;
	wxDataViewItem GetTopItem() const;

	// I change this method to un const because in the tree view,
	// the displaying number of the tree are changing along with the
	// expanding/collapsing of the tree nodes
	unsigned int GetLastVisibleRow();
	unsigned int GetLastFullyVisibleRow();
	unsigned int GetRowCount() const;

	const wxSelectionStore& GetSelections() const { return m_selection; }
	void ClearSelection() { m_selection.SelectRange(0, GetRowCount() - 1, false); }
	void Select(const wxArrayInt& aSelections);

	void SelectAllRows()
	{
		m_selection.SelectRange(0, GetRowCount() - 1);
		Refresh();
	}

	// If a valid row is specified and it was previously selected, it is left
	// selected and the function returns false. Otherwise, i.e. if there is
	// really no selection left in the control, it returns true.
	bool UnselectAllRows(unsigned int except = (unsigned int)-1);

	void SelectRow(unsigned int row, bool on);
	void SelectRows(unsigned int from, unsigned int to);
	void ReverseRowSelection(unsigned int row);
	bool IsRowSelected(unsigned int row);
	void SendSelectionChangedEvent(const wxDataViewItem& item);

	void RefreshRow(unsigned int row) { RefreshRows(row, row); }
	void RefreshRows(unsigned int from, unsigned int to);
	void RefreshRowsAfter(unsigned int firstRow);

	// returns the colour to be used for drawing the rules
	wxColour GetRuleColour() const
	{
		return wxSystemSettings::GetColour(wxSYS_COLOUR_3DLIGHT);
	}

	wxRect GetLinesRect(unsigned int rowFrom, unsigned int rowTo) const;

	int GetLineStart(unsigned int row) const;  // row * m_lineHeight in fixed mode
	int GetLineHeight(unsigned int row) const; // m_lineHeight in fixed mode
	int GetLineAt(unsigned int y) const;       // y / m_lineHeight in fixed mode
	int QueryAndCacheLineHeight(unsigned int row, wxDataViewItem item) const;

	void SetRowHeight(int lineHeight) { m_lineHeight = lineHeight; }
	int GetRowHeight() const { return m_lineHeight; }
	int GetDefaultRowHeight() const;

	// Some useful functions for row and item mapping
	wxDataViewItem GetItemByRow(unsigned int row) const;
	int GetRowByItem(const wxDataViewItem & item,
		WalkFlags flags = Walk_All) const;

	CDataViewTreeNode * GetTreeNodeByRow(unsigned int row) const;
	// We did not need this temporarily
	// CDataViewTreeNode * GetTreeNodeByItem( const wxDataViewItem & item );

	// Methods for building the mapping tree
	void BuildTree(CDataViewModel  * model);
	void DestroyTree();
	void HitTest(const wxPoint & point, wxDataViewItem & item, CDataViewColumn* &column);
	wxRect GetItemRect(const wxDataViewItem & item, const CDataViewColumn* column);

	void Expand(unsigned int row, bool expandChildren = false);
	void Collapse(unsigned int row);
	bool IsExpanded(unsigned int row) const;
	bool HasChildren(unsigned int row) const;

#if wxUSE_DRAG_AND_DROP
	enum DropHint
	{
		DropHint_None = 0,
		DropHint_Inside,
		DropHint_Below,
		DropHint_Above
	};
	struct DropItemInfo
	{
		unsigned int        m_row;
		DropHint            m_hint;

		wxDataViewItem      m_item;
		int                 m_proposedDropIndex;
		int                 m_indentLevel;

		DropItemInfo()
			: m_row(static_cast<unsigned int>(-1))
			, m_hint(DropHint_None)
			, m_item(NULL)
			, m_proposedDropIndex(-1)
			, m_indentLevel(-1)
		{
		}
	};

	bool EnableDragSource(const wxDataFormat &format);
	bool EnableDropTarget(const wxDataFormat &format);

	void RefreshDropHint();
	void RemoveDropHint();
	DropItemInfo GetDropItemInfo(const wxCoord x, const wxCoord y);
	wxDragResult OnDragOver(wxDataFormat format, wxCoord x, wxCoord y, wxDragResult def);
	bool OnDrop(wxDataFormat format, wxCoord x, wxCoord y);
	wxDragResult OnData(wxDataFormat format, wxCoord x, wxCoord y, wxDragResult def);
	void OnLeave();
#endif // wxUSE_DRAG_AND_DROP

	void OnColumnsCountChanged();

	// Adjust last column to window size
	void UpdateColumnSizes();

	// Called by CDataViewCtrl and our own OnRenameTimer() to start edit the
	// specified item in the given column.
	void StartEditing(const wxDataViewItem& item, const CDataViewColumn* col);
	void FinishEditing();
	bool HasEditableColumn(const wxDataViewItem& item) const
	{
		return FindColumnForEditing(item, wxDATAVIEW_CELL_EDITABLE) != NULL;
	}

private:
	void InvalidateCount() { m_count = -1; }
	void UpdateCount(int count)
	{
		m_count = count;
		m_selection.SetItemCount(count);
	}

	int RecalculateCount() const;

	// Return false only if the event was vetoed by its handler.
	bool SendExpanderEvent(wxEventType type, const wxDataViewItem& item);

	CDataViewTreeNode * FindNode(const wxDataViewItem & item);

	CDataViewColumn *FindColumnForEditing(const wxDataViewItem& item, wxDataViewCellMode mode) const;

	bool IsCellEditableInMode(const wxDataViewItem& item, const CDataViewColumn *col, wxDataViewCellMode mode) const;

	void DrawCellBackground(wxDataViewRenderer* cell, wxDC& dc, const wxRect& rect);

	// Common part of {Item,Value}Changed(): if view_column is wxNOT_FOUND,
	// assumes that all columns were modified, otherwise just this one.
	bool DoItemChanged(const wxDataViewItem& item, int view_column);

	// Return whether the item has at most one column with a value.
	bool IsItemSingleValued(const wxDataViewItem& item) const
	{
		bool hadColumnWithValue = false;
		const unsigned int cols = GetOwner()->GetColumnCount();
		const CDataViewModel* const model = GetModel();
		for (unsigned int i = 0; i < cols; i++)
		{
			if (model->HasValue(item, i))
			{
				if (hadColumnWithValue)
					return false;
				hadColumnWithValue = true;
			}
		}

		return true;
	}

	// Find the first column with a value in it.
	CDataViewColumn* FindFirstColumnWithValue(const wxDataViewItem& item) const
	{
		const unsigned int cols = GetOwner()->GetColumnCount();
		const CDataViewModel* const model = GetModel();
		for (unsigned int i = 0; i < cols; i++)
		{
			if (model->HasValue(item, i))
				return GetOwner()->GetColumnAt(i);
		}

		return NULL;
	}

	// Helper of public Expand(), must be called with a valid node.
	void DoExpand(CDataViewTreeNode* node, unsigned int row, bool expandChildren);

private:
	CDataViewCtrl             *m_owner;
	int                         m_lineHeight;
	bool                        m_dirty;

	CDataViewColumn           *m_currentCol;
	unsigned int                m_currentRow;
	wxSelectionStore            m_selection;

	CDataViewRenameTimer      *m_renameTimer;
	bool                        m_lastOnSame;

	bool                        m_hasFocus;
	bool                        m_useCellFocus;
	bool                        m_currentColSetByKeyboard;
	HeightCache                *m_rowHeightCache;

#if wxUSE_DRAG_AND_DROP
	int                         m_dragCount;
	wxPoint                     m_dragStart;

	bool                        m_dragEnabled;
	wxDataFormat                m_dragFormat;

	bool                        m_dropEnabled;
	wxDataFormat                m_dropFormat;
	DropItemInfo                m_dropItemInfo;
#endif // wxUSE_DRAG_AND_DROP

	// for double click logic
	unsigned int m_lineLastClicked,
		m_lineBeforeLastClicked,
		m_lineSelectSingleOnUp;

	// the pen used to draw horiz/vertical rules
	wxPen m_penRule;

	// This is the tree structure of the model
	CDataViewTreeNode * m_root;
	int m_count;

	// This is the tree node under the cursor
	CDataViewTreeNode * m_underMouse;

	// The control used for editing or NULL.
	wxWeakRef<wxWindow> m_editorCtrl;

	// Id m_editorCtrl is non-NULL, pointer to the associated renderer.
	wxDataViewRenderer* m_editorRenderer;

private:
	wxDECLARE_DYNAMIC_CLASS(CDataViewMainWindow);
	wxDECLARE_EVENT_TABLE();
};

// ---------------------------------------------------------
// CGenericDataViewModelNotifier
// ---------------------------------------------------------

class CGenericDataViewModelNotifier : public CDataViewModelNotifier
{
public:
	CGenericDataViewModelNotifier(CDataViewMainWindow *mainWindow)
	{
		m_mainWindow = mainWindow;
	}

	virtual bool ItemAdded(const wxDataViewItem & parent, const wxDataViewItem & item) wxOVERRIDE
	{
		return m_mainWindow->ItemAdded(parent, item);
	}
	virtual bool ItemDeleted(const wxDataViewItem &parent, const wxDataViewItem &item) wxOVERRIDE
	{
		return m_mainWindow->ItemDeleted(parent, item);
	}
	virtual bool ItemChanged(const wxDataViewItem & item) wxOVERRIDE
	{
		return m_mainWindow->ItemChanged(item);
	}
	virtual bool ValueChanged(const wxDataViewItem & item, unsigned int col) wxOVERRIDE
	{
		return m_mainWindow->ValueChanged(item, col);
	}
	virtual bool Cleared() wxOVERRIDE
	{
		return m_mainWindow->Cleared();
	}
	virtual void Resort() wxOVERRIDE
	{
		m_mainWindow->Resort();
	}

	CDataViewMainWindow    *m_mainWindow;
};

//-----------------------------------------------------------------------------
// CDataViewDropTarget
//-----------------------------------------------------------------------------

#if wxUSE_DRAG_AND_DROP

class CBitmapCanvas : public wxWindow
{
public:
	CBitmapCanvas(wxWindow *parent, const wxBitmap &bitmap, const wxSize &size) :
		wxWindow(parent, wxID_ANY, wxPoint(0, 0), size)
		, m_bitmap(bitmap)
	{
		Bind(wxEVT_PAINT, &CBitmapCanvas::OnPaint, this);
	}

	void OnPaint(wxPaintEvent &WXUNUSED(event))
	{
		wxPaintDC dc(this);
		dc.DrawBitmap(m_bitmap, 0, 0);
	}

	wxBitmap m_bitmap;
};

class CDataViewDropSource : public wxDropSource
{
public:
	CDataViewDropSource(CDataViewMainWindow *win, unsigned int row) :
		wxDropSource(win)
	{
		m_win = win;
		m_row = row;
		m_hint = NULL;
	}

	~CDataViewDropSource()
	{
		delete m_hint;
	}

	virtual bool GiveFeedback(wxDragResult WXUNUSED(effect)) wxOVERRIDE
	{
		wxPoint pos = wxGetMousePosition();

		if (!m_hint)
		{
			int liney = m_win->GetLineStart(m_row);
			int linex = 0;
			m_win->GetOwner()->CalcUnscrolledPosition(0, liney, NULL, &liney);
			m_win->ClientToScreen(&linex, &liney);
			m_dist_x = pos.x - linex;
			m_dist_y = pos.y - liney;

			int indent = 0;
			wxBitmap ib = m_win->CreateItemBitmap(m_row, indent);
			m_dist_x -= indent;
			m_hint = new wxFrame(m_win->GetParent(), wxID_ANY, wxEmptyString,
				wxPoint(pos.x - m_dist_x, pos.y + 5),
				ib.GetSize(),
				wxFRAME_TOOL_WINDOW |
				wxFRAME_FLOAT_ON_PARENT |
				wxFRAME_NO_TASKBAR |
				wxNO_BORDER);
			new CBitmapCanvas(m_hint, ib, ib.GetSize());
			m_hint->Show();
		}
		else
		{
			m_hint->Move(pos.x - m_dist_x, pos.y + 5);
			m_hint->SetTransparent(128);
		}

		return false;
	}

	CDataViewMainWindow   *m_win;
	unsigned int            m_row;
	wxFrame                *m_hint;
	int m_dist_x, m_dist_y;
};


class CDataViewDropTarget : public wxDropTarget
{
public:
	CDataViewDropTarget(wxDataObject *obj, CDataViewMainWindow *win) :
		wxDropTarget(obj)
	{
		m_win = win;
	}

	virtual wxDragResult OnDragOver(wxCoord x, wxCoord y, wxDragResult def) wxOVERRIDE
	{
		wxDataFormat format = GetMatchingPair();
		if (format == wxDF_INVALID)
			return wxDragNone;
		return m_win->OnDragOver(format, x, y, def);
	}

	virtual bool OnDrop(wxCoord x, wxCoord y) wxOVERRIDE
	{
		wxDataFormat format = GetMatchingPair();
		if (format == wxDF_INVALID)
			return false;
		return m_win->OnDrop(format, x, y);
	}

	virtual wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) wxOVERRIDE
	{
		wxDataFormat format = GetMatchingPair();
		if (format == wxDF_INVALID)
			return wxDragNone;
		if (!GetData())
			return wxDragNone;
		return m_win->OnData(format, x, y, def);
	}

	virtual void OnLeave() wxOVERRIDE
	{
		m_win->OnLeave();
	}

	CDataViewMainWindow   *m_win;
};

#endif // wxUSE_DRAG_AND_DROP

//-----------------------------------------------------------------------------
// CDataViewRenameTimer
//-----------------------------------------------------------------------------

CDataViewRenameTimer::CDataViewRenameTimer(CDataViewMainWindow *owner)
{
	m_owner = owner;
}

void CDataViewRenameTimer::Notify()
{
	m_owner->OnRenameTimer();
}



// ----------------------------------------------------------------------------
// CDataViewTreeNode
// ----------------------------------------------------------------------------

namespace
{

	// Comparator used for sorting the tree nodes using the model-defined sort
	// order and also for performing binary search in our own code.
	class wxGenericTreeModelNodeCmp
	{
	public:
		wxGenericTreeModelNodeCmp(CDataViewMainWindow* window,
			const SortOrder& sortOrder)
			: m_model(window->GetModel()),
			m_sortOrder(sortOrder)
		{
			wxASSERT_MSG(!m_sortOrder.IsNone(), "should have sort order");
		}

		// Return negative, zero or positive value depending on whether the first
		// item is less than, equal to or greater than the second one.
		int Compare(CDataViewTreeNode* first, CDataViewTreeNode* second) const
		{
			return m_model->Compare(first->GetItem(), second->GetItem(),
				m_sortOrder.GetColumn(),
				m_sortOrder.IsAscending());
		}

		// Return true if the items are (strictly) in order, i.e. the first item is
		// less than the second one. This is used by std::sort().
		bool operator()(CDataViewTreeNode* first, CDataViewTreeNode* second) const
		{
			return Compare(first, second) < 0;
		}

	private:
		CDataViewModel* const m_model;
		const SortOrder m_sortOrder;
	};

} // anonymous namespace

void CDataViewTreeNode::InsertChild(CDataViewMainWindow* window,
	CDataViewTreeNode *node, unsigned index)
{
	if (!m_branchData)
		m_branchData = new BranchNodeData;

	const SortOrder sortOrder = window->GetSortOrder();

	// Flag indicating whether we should retain existing sorted list when
	// inserting the child node.
	bool insertSorted = false;

	if (sortOrder.IsNone())
	{
		// We should insert assuming an unsorted list. This will cause the
		// child list to lose the current sort order, if any.
		m_branchData->sortOrder = SortOrder();
	}
	else if (m_branchData->children.empty())
	{
		if (m_branchData->open)
		{
			// We don't need to search for the right place to insert the first
			// item (there is only one), but we do need to remember the sort
			// order to use for the subsequent ones.
			m_branchData->sortOrder = sortOrder;
		}
		else
		{
			// We're inserting the first child of a closed node. We can choose
			// whether to consider this empty child list sorted or unsorted.
			// By choosing unsorted, we postpone comparisons until the parent
			// node is opened in the view, which may be never.
			m_branchData->sortOrder = SortOrder();
		}
	}
	else if (m_branchData->open)
	{
		// For open branches, children should be already sorted.
		wxASSERT_MSG(m_branchData->sortOrder == sortOrder,
			wxS("Logic error in wxDVC sorting code"));

		// We can use fast insertion.
		insertSorted = true;
	}
	else if (m_branchData->sortOrder == sortOrder)
	{
		// The children are already sorted by the correct criteria (because
		// the node must have been opened in the same time in the past). Even
		// though it is closed now, we still insert in sort order to avoid a
		// later resort.
		insertSorted = true;
	}
	else
	{
		// The children of this closed node aren't sorted by the correct
		// criteria, so we just insert unsorted.
		m_branchData->sortOrder = SortOrder();
	}


	if (insertSorted)
	{
		// Use binary search to find the correct position to insert at.
		wxGenericTreeModelNodeCmp cmp(window, sortOrder);
		int lo = 0, hi = m_branchData->children.size();
		while (lo < hi)
		{
			int mid = lo + (hi - lo) / 2;
			int r = cmp.Compare(node, m_branchData->children[mid]);
			if (r < 0)
				hi = mid;
			else if (r > 0)
				lo = mid + 1;
			else
				lo = hi = mid;
		}
		m_branchData->InsertChild(node, lo);
	}
	else
	{
		m_branchData->InsertChild(node, index);
	}
}


void CDataViewTreeNode::Resort(CDataViewMainWindow* window)
{
	if (!m_branchData)
		return;

	// No reason to sort a closed node.
	if (!m_branchData->open)
		return;

	const SortOrder sortOrder = window->GetSortOrder();
	if (!sortOrder.IsNone())
	{
		CDataViewTreeNodes& nodes = m_branchData->children;

		// When sorting by column value, we can skip resorting entirely if the
		// same sort order was used previously. However we can't do this when
		// using model-specific sort order, which can change at any time.
		if (m_branchData->sortOrder != sortOrder || !sortOrder.UsesColumn())
		{
			std::sort(m_branchData->children.begin(),
				m_branchData->children.end(),
				wxGenericTreeModelNodeCmp(window, sortOrder));

			m_branchData->sortOrder = sortOrder;
		}

		// There may be open child nodes that also need a resort.
		int len = nodes.size();
		for (int i = 0; i < len; i++)
		{
			if (nodes[i]->HasChildren())
				nodes[i]->Resort(window);
		}
	}
}


void
CDataViewTreeNode::PutChildInSortOrder(CDataViewMainWindow* window,
	CDataViewTreeNode* childNode)
{
	// The childNode has changed, and may need to be moved to another location
	// in the sorted child list.

	if (!m_branchData)
		return;
	if (!m_branchData->open)
		return;
	if (m_branchData->sortOrder.IsNone())
		return;

	CDataViewTreeNodes& nodes = m_branchData->children;

	// This is more than an optimization, the code below assumes that 1 is a
	// valid index.
	if (nodes.size() == 1)
		return;

	// We should already be sorted in the right order.
	wxASSERT(m_branchData->sortOrder == window->GetSortOrder());

	// First find the node in the current child list
	int hi = nodes.size();
	int oldLocation = wxNOT_FOUND;
	for (int index = 0; index < hi; ++index)
	{
		if (nodes[index] == childNode)
		{
			oldLocation = index;
			break;
		}
	}
	wxCHECK_RET(oldLocation >= 0, "not our child?");

	wxGenericTreeModelNodeCmp cmp(window, m_branchData->sortOrder);

	// Check if we actually need to move the node.
	bool locationChanged = false;

	if (oldLocation == 0)
	{
		// Compare with the next item (as we return early in the case of only a
		// single child, we know that there is one) to check if the item is now
		// out of order.
		if (!cmp(childNode, nodes[1]))
			locationChanged = true;
	}
	else // Compare with the previous item.
	{
		if (!cmp(nodes[oldLocation - 1], childNode))
			locationChanged = true;
	}

	if (!locationChanged)
		return;

	// Remove and reinsert the node in the child list
	m_branchData->RemoveChild(oldLocation);
	hi = nodes.size();
	int lo = 0;
	while (lo < hi)
	{
		int mid = lo + (hi - lo) / 2;
		int r = cmp.Compare(childNode, m_branchData->children[mid]);
		if (r < 0)
			hi = mid;
		else if (r > 0)
			lo = mid + 1;
		else
			lo = hi = mid;
	}
	m_branchData->InsertChild(childNode, lo);

	// Make sure the change is actually shown right away
	window->UpdateDisplay();
}


//-----------------------------------------------------------------------------
// CDataViewMainWindow
//-----------------------------------------------------------------------------

// The tree building helper, declared firstly
static void BuildTreeHelper(CDataViewMainWindow *window,
	const CDataViewModel *model,
	const wxDataViewItem & item,
	CDataViewTreeNode * node);

wxIMPLEMENT_ABSTRACT_CLASS(CDataViewMainWindow, wxWindow);

wxBEGIN_EVENT_TABLE(CDataViewMainWindow, wxWindow)
EVT_PAINT(CDataViewMainWindow::OnPaint)
EVT_MOUSE_EVENTS(CDataViewMainWindow::OnMouse)
EVT_SET_FOCUS(CDataViewMainWindow::OnSetFocus)
EVT_KILL_FOCUS(CDataViewMainWindow::OnKillFocus)
EVT_CHAR_HOOK(CDataViewMainWindow::OnCharHook)
EVT_CHAR(CDataViewMainWindow::OnChar)
wxEND_EVENT_TABLE()

CDataViewMainWindow::CDataViewMainWindow(CDataViewCtrl *parent, wxWindowID id,
	const wxPoint &pos, const wxSize &size, const wxString &name)
{
	// We want to use a specific class name for this window in wxMSW to make it
	// possible to configure screen readers to handle it specifically.
#ifdef __WXMSW__
	CreateUsingMSWClass
	(
		wxApp::GetRegisteredClassName
		(
			wxT("wxDataView"),
			-1, // no specific background brush
			0, // no special styles neither
			wxApp::RegClass_OnlyNR
		),
		parent, id, pos, size, wxWANTS_CHARS | wxBORDER_NONE, name
	);
#else
	Create(parent, id, pos, size, wxWANTS_CHARS | wxBORDER_NONE, name);
#endif

	SetOwner(parent);

	m_editorRenderer = NULL;

	m_lastOnSame = false;
	m_renameTimer = new CDataViewRenameTimer(this);

	// TODO: user better initial values/nothing selected
	m_currentCol = NULL;
	m_currentColSetByKeyboard = false;
	m_useCellFocus = false;
	m_currentRow = (unsigned)-1;
	m_lineHeight = GetDefaultRowHeight();
	if (GetOwner()->HasFlag(wxDV_VARIABLE_LINE_HEIGHT))
	{
		m_rowHeightCache = new HeightCache();
	}
	else
	{
		m_rowHeightCache = NULL;
	}

#if wxUSE_DRAG_AND_DROP
	m_dragCount = 0;
	m_dragStart = wxPoint(0, 0);

	m_dragEnabled = false;
	m_dropEnabled = false;
	m_dropItemInfo = DropItemInfo();
#endif // wxUSE_DRAG_AND_DROP

	m_lineLastClicked = (unsigned int)-1;
	m_lineBeforeLastClicked = (unsigned int)-1;
	m_lineSelectSingleOnUp = (unsigned int)-1;

	m_hasFocus = false;

	SetBackgroundColour(*wxWHITE);

	SetBackgroundStyle(wxBG_STYLE_PAINT);

	m_penRule = wxPen(GetRuleColour());

	m_root = CDataViewTreeNode::CreateRootNode();

	// Make m_count = -1 will cause the class recaculate the real displaying number of rows.
	m_count = -1;
	m_underMouse = NULL;

	UpdateDisplay();
}

CDataViewMainWindow::~CDataViewMainWindow()
{
	DestroyTree();
	delete m_renameTimer;
	delete m_rowHeightCache;
}


int CDataViewMainWindow::GetDefaultRowHeight() const
{
#ifdef __WXMSW__
	// We would like to use the same line height that Explorer uses. This is
	// different from standard ListView control since Vista.
	if (wxGetWinVersion() >= wxWinVersion_Vista)
		return wxMax(16, GetCharHeight()) + 6; // 16 = mini icon height
	else
#endif // __WXMSW__
		return wxMax(16, GetCharHeight()) + 1; // 16 = mini icon height
}



#if wxUSE_DRAG_AND_DROP
bool CDataViewMainWindow::EnableDragSource(const wxDataFormat &format)
{
	m_dragFormat = format;
	m_dragEnabled = format != wxDF_INVALID;

	return true;
}

bool CDataViewMainWindow::EnableDropTarget(const wxDataFormat &format)
{
	m_dropFormat = format;
	m_dropEnabled = format != wxDF_INVALID;

	if (m_dropEnabled)
		SetDropTarget(new CDataViewDropTarget(new wxCustomDataObject(format), this));

	return true;
}

void CDataViewMainWindow::RefreshDropHint()
{
	const unsigned row = m_dropItemInfo.m_row;

	switch (m_dropItemInfo.m_hint)
	{
	case DropHint_None:
		break;

	case DropHint_Inside:
		RefreshRow(row);
		break;

	case DropHint_Above:
		RefreshRows(row == 0 ? 0 : row - 1, row);
		break;

	case DropHint_Below:
		// It's not a problem here if row+1 is out of range, RefreshRows()
		// allows this.
		RefreshRows(row, row + 1);
		break;
	}
}

void CDataViewMainWindow::RemoveDropHint()
{
	RefreshDropHint();

	m_dropItemInfo = DropItemInfo();
}

CDataViewMainWindow::DropItemInfo CDataViewMainWindow::GetDropItemInfo(const wxCoord x, const wxCoord y)
{
	DropItemInfo dropItemInfo;

	int xx = x;
	int yy = y;
	m_owner->CalcUnscrolledPosition(xx, yy, &xx, &yy);

	unsigned int row = GetLineAt(yy);
	dropItemInfo.m_row = row;

	if (row >= GetRowCount() || xx > GetEndOfLastCol())
		return dropItemInfo;

	if (IsVirtualList())
	{
		dropItemInfo.m_item = GetItemByRow(row);

		if (dropItemInfo.m_item.IsOk())
			dropItemInfo.m_hint = DropHint_Inside;
	}
	else
	{
		CDataViewTreeNode* node = GetTreeNodeByRow(row);
		if (!node)
			return dropItemInfo;

		dropItemInfo.m_item = node->GetItem();

		const int itemStart = GetLineStart(row);
		const int itemHeight = GetLineHeight(row);

		// 15% is an arbitrarily chosen threshold here, which could be changed
		// or made configurable if really needed.
		static const double UPPER_ITEM_PART = 0.15;

		bool insertAbove = yy - itemStart < itemHeight*UPPER_ITEM_PART;
		if (insertAbove)
		{
			// Can be treated as 'insertBelow" with the small difference:
			node = GetTreeNodeByRow(row - 1);   // We need the node from the previous row

			dropItemInfo.m_hint = DropHint_Above;

			if (!node)
			{
				// Seems to be dropped in the root node

				dropItemInfo.m_indentLevel = 0;
				dropItemInfo.m_proposedDropIndex = 0;
				dropItemInfo.m_item = wxDataViewItem();

				return dropItemInfo;
			}

		}

		bool insertBelow = yy - itemStart > itemHeight*(1.0 - UPPER_ITEM_PART);
		if (insertBelow)
			dropItemInfo.m_hint = DropHint_Below;

		if (insertBelow || insertAbove)
		{
			// Insert inside the 'item' or after (below) it. Depends on:
			// 1 - the 'item' is a container
			// 2 - item's child index in it's parent (the last in the parent or not)
			// 3 - expanded (opened) or not
			// 4 - mouse x position

			int xStart = 0;     // Expander column x position start
			CDataViewColumn* const expander = GetExpanderColumnOrFirstOne(GetOwner());
			for (unsigned int i = 0; i < GetOwner()->GetColumnCount(); i++)
			{
				CDataViewColumn* col = GetOwner()->GetColumnAt(i);
				if (col->IsHidden())
					continue;   // skip it!

				if (col == expander)
					break;

				xStart += col->GetWidth();
			}

			const int expanderWidth = wxRendererNative::Get().GetExpanderSize(this).GetWidth();

			int level = node->GetIndentLevel();

			CDataViewTreeNode* prevAscendNode = node;
			CDataViewTreeNode* ascendNode = node;
			while (ascendNode != NULL)
			{
				dropItemInfo.m_indentLevel = level + 1;

				if (m_owner->GetModel()->IsContainer(ascendNode->GetItem()))
				{
					// Item can be inserted
					dropItemInfo.m_item = ascendNode->GetItem();

					int itemPosition = ascendNode->FindChildByItem(prevAscendNode->GetItem());
					if (itemPosition == wxNOT_FOUND)
						itemPosition = 0;
					else
						itemPosition++;

					dropItemInfo.m_proposedDropIndex = itemPosition;

					// We must break the loop if the applied node is expanded
					// (opened) and the proposed drop position is not the last
					// in this node.
					if (ascendNode->IsOpen())
					{
						const size_t lastPos = ascendNode->GetChildNodes().size();
						if (static_cast<size_t>(itemPosition) != lastPos)
							break;
					}

					int indent = GetOwner()->GetIndent()*level + expanderWidth;

					if (xx >= xStart + indent)
						break;
				}

				prevAscendNode = ascendNode;
				ascendNode = ascendNode->GetParent();
				--level;
			}
		}
		else
		{
			dropItemInfo.m_hint = DropHint_Inside;
		}
	}

	return dropItemInfo;
}


wxDragResult CDataViewMainWindow::OnDragOver(wxDataFormat format, wxCoord x,
	wxCoord y, wxDragResult def)
{
	DropItemInfo nextDropItemInfo = GetDropItemInfo(x, y);

	CDataViewEvent event(wxEVT_DATAVIEW_ITEM_DROP_POSSIBLE, m_owner, nextDropItemInfo.m_item);
	event.SetProposedDropIndex(nextDropItemInfo.m_proposedDropIndex);
	event.SetDataFormat(format);
	event.SetDropEffect(def);

	wxDragResult result = def;

	if (m_owner->HandleWindowEvent(event) && event.IsAllowed())
	{
		// Processing handled event

		result = event.GetDropEffect();
		switch (result)
		{
		case wxDragCopy:
		case wxDragMove:
		case wxDragLink:
			break;

		case wxDragNone:
		case wxDragCancel:
		case wxDragError:
		{
			RemoveDropHint();
			return result;
		}
		}
	}
	else
	{
		RemoveDropHint();
		return wxDragNone;
	}

	if (nextDropItemInfo.m_hint != DropHint_None)
	{
		if (m_dropItemInfo.m_hint != nextDropItemInfo.m_hint ||
			m_dropItemInfo.m_row != nextDropItemInfo.m_row)
		{
			RefreshDropHint();   // refresh previous rows
		}

		m_dropItemInfo.m_hint = nextDropItemInfo.m_hint;
		m_dropItemInfo.m_row = nextDropItemInfo.m_row;

		RefreshDropHint();
	}
	else
	{
		RemoveDropHint();
	}

	m_dropItemInfo = nextDropItemInfo;

	return result;
}

bool CDataViewMainWindow::OnDrop(wxDataFormat format, wxCoord x, wxCoord y)
{
	RemoveDropHint();

	DropItemInfo dropItemInfo = GetDropItemInfo(x, y);

	CDataViewEvent event(wxEVT_DATAVIEW_ITEM_DROP_POSSIBLE, m_owner, dropItemInfo.m_item);
	event.SetProposedDropIndex(dropItemInfo.m_proposedDropIndex);
	event.SetDataFormat(format);
	if (!m_owner->HandleWindowEvent(event) || !event.IsAllowed())
		return false;

	return true;
}

wxDragResult CDataViewMainWindow::OnData(wxDataFormat format, wxCoord x, wxCoord y,
	wxDragResult def)
{
	DropItemInfo dropItemInfo = GetDropItemInfo(x, y);

	wxCustomDataObject *obj = (wxCustomDataObject *)GetDropTarget()->GetDataObject();

	CDataViewEvent event(wxEVT_DATAVIEW_ITEM_DROP, m_owner, dropItemInfo.m_item);
	event.SetProposedDropIndex(dropItemInfo.m_proposedDropIndex);
	event.SetDataFormat(format);
	event.SetDataSize(obj->GetSize());
	event.SetDataBuffer(obj->GetData());
	event.SetDropEffect(def);
	if (!m_owner->HandleWindowEvent(event) || !event.IsAllowed())
		return wxDragNone;

	return def;
}

void CDataViewMainWindow::OnLeave()
{
	RemoveDropHint();
}

wxBitmap CDataViewMainWindow::CreateItemBitmap(unsigned int row, int &indent)
{
	int height = GetLineHeight(row);
	int width = 0;
	unsigned int cols = GetOwner()->GetColumnCount();
	unsigned int col;
	for (col = 0; col < cols; col++)
	{
		CDataViewColumn *column = GetOwner()->GetColumnAt(col);
		if (column->IsHidden())
			continue;      // skip it!
		width += column->GetWidth();
	}

	indent = 0;
	if (!IsList())
	{
		CDataViewTreeNode *node = GetTreeNodeByRow(row);
		indent = GetOwner()->GetIndent() * node->GetIndentLevel();
		indent += wxRendererNative::Get().GetExpanderSize(this).GetWidth();
	}
	width -= indent;

	wxBitmap bitmap(width, height);
	wxMemoryDC dc(bitmap);
	dc.SetFont(GetFont());
	dc.SetPen(*wxBLACK_PEN);
	dc.SetBrush(*wxWHITE_BRUSH);
	dc.DrawRectangle(0, 0, width, height);

	CDataViewModel *model = m_owner->GetModel();

	CDataViewColumn * const
		expander = GetExpanderColumnOrFirstOne(GetOwner());

	int x = 0;
	for (col = 0; col < cols; col++)
	{
		CDataViewColumn *column = GetOwner()->GetColumnAt(col);
		wxDataViewRenderer *cell = column->GetRenderer();

		if (column->IsHidden())
			continue;       // skip it!

		width = column->GetWidth();

		if (column == expander)
			width -= indent;

		wxDataViewItem item = GetItemByRow(row);
		cell->PrepareForItem(model, item, column->GetModelColumn());

		wxRect item_rect(x, 0, width, height);
		item_rect.Deflate(PADDING_RIGHTLEFT, 0);

		// dc.SetClippingRegion( item_rect );
		cell->WXCallRender(item_rect, &dc, 0);
		// dc.DestroyClippingRegion();

		x += width;
	}

	return bitmap;
}

#endif // wxUSE_DRAG_AND_DROP

void CDataViewMainWindow::OnPaint(wxPaintEvent &WXUNUSED(event))
{
	CDataViewModel *model = GetModel();
	wxAutoBufferedPaintDC dc(this);

	const wxSize size = GetClientSize();

	dc.SetBrush(GetOwner()->GetBackgroundColour());
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(size);

	if (IsEmpty())
	{
		// No items to draw.
		return;
	}

	// prepare the DC
	GetOwner()->PrepareDC(dc);
	dc.SetFont(GetFont());

	wxRect update = GetUpdateRegion().GetBox();
	m_owner->CalcUnscrolledPosition(update.x, update.y, &update.x, &update.y);

	// compute which items needs to be redrawn
	unsigned int item_start = GetLineAt(wxMax(0, update.y));
	unsigned int item_count =
		wxMin((int)(GetLineAt(wxMax(0, update.y + update.height)) - item_start + 1),
		(int)(GetRowCount() - item_start));
	unsigned int item_last = item_start + item_count;

	// Send the event to CDataViewCtrl itself.
	CDataViewEvent cache_event(wxEVT_DATAVIEW_CACHE_HINT, m_owner, NULL);
	cache_event.SetCache(item_start, item_last - 1);
	m_owner->ProcessWindowEvent(cache_event);

	// compute which columns needs to be redrawn
	unsigned int cols = GetOwner()->GetColumnCount();
	if (!cols)
	{
		// we assume that we have at least one column below and painting an
		// empty control is unnecessary anyhow
		return;
	}

	unsigned int col_start = 0;
	unsigned int x_start;
	for (x_start = 0; col_start < cols; col_start++)
	{
		CDataViewColumn *col = GetOwner()->GetColumnAt(col_start);
		if (col->IsHidden())
			continue;      // skip it!

		unsigned int w = col->GetWidth();
		if (x_start + w >= (unsigned int)update.x)
			break;

		x_start += w;
	}

	unsigned int col_last = col_start;
	unsigned int x_last = x_start;
	for (; col_last < cols; col_last++)
	{
		CDataViewColumn *col = GetOwner()->GetColumnAt(col_last);
		if (col->IsHidden())
			continue;      // skip it!

		if (x_last > (unsigned int)update.GetRight())
			break;

		x_last += col->GetWidth();
	}

	// Instead of calling GetLineStart() for each line from the first to the
	// last one, we will compute the starts of the lines as we iterate over
	// them starting from this one, as this is much more efficient when using
	// wxDV_VARIABLE_LINE_HEIGHT (and doesn't really change anything when not
	// using it, so there is no need to use two different approaches).
	const unsigned int first_line_start = GetLineStart(item_start);

	// Draw background of alternate rows specially if required
	if (m_owner->HasFlag(wxDV_ROW_LINES))
	{
		wxColour altRowColour = m_owner->m_alternateRowColour;
		if (!altRowColour.IsOk())
		{
			// Determine the alternate rows colour automatically from the
			// background colour.
			const wxColour bgColour = m_owner->GetBackgroundColour();

			// Depending on the background, alternate row color
			// will be 3% more dark or 50% brighter.
			int alpha = bgColour.GetRGB() > 0x808080 ? 97 : 150;
			altRowColour = bgColour.ChangeLightness(alpha);
		}

		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(altRowColour));

		// We only need to draw the visible part, so limit the rectangle to it.
		const int xRect = m_owner->CalcUnscrolledPosition(wxPoint(0, 0)).x;
		const int widthRect = size.x;
		unsigned int cur_line_start = first_line_start;
		for (unsigned int item = item_start; item < item_last; item++)
		{
			const int h = GetLineHeight(item);
			if (item % 2)
			{
				dc.DrawRectangle(xRect, cur_line_start, widthRect, h);
			}
			cur_line_start += h;
		}
	}

	// Draw horizontal rules if required
	if (m_owner->HasFlag(wxDV_HORIZ_RULES))
	{
		dc.SetPen(m_penRule);
		dc.SetBrush(*wxTRANSPARENT_BRUSH);

		unsigned int cur_line_start = first_line_start;
		for (unsigned int i = item_start; i <= item_last; i++)
		{
			const int h = GetLineHeight(i);
			dc.DrawLine(x_start, cur_line_start, x_last, cur_line_start);
			cur_line_start += h;
		}
	}

	// Draw vertical rules if required
	if (m_owner->HasFlag(wxDV_VERT_RULES))
	{
		dc.SetPen(m_penRule);
		dc.SetBrush(*wxTRANSPARENT_BRUSH);

		// NB: Vertical rules are drawn in the last pixel of a column so that
		//     they align perfectly with native MSW wxHeaderCtrl as well as for
		//     consistency with MSW native list control. There's no vertical
		//     rule at the most-left side of the control.

		int x = x_start - 1;
		int line_last = GetLineStart(item_last);
		for (unsigned int i = col_start; i < col_last; i++)
		{
			CDataViewColumn *col = GetOwner()->GetColumnAt(i);
			if (col->IsHidden())
				continue;       // skip it

			x += col->GetWidth();

			dc.DrawLine(x, first_line_start,
				x, line_last);
		}
	}

	// redraw the background for the items which are selected/current
	unsigned int cur_line_start = first_line_start;
	for (unsigned int item = item_start; item < item_last; item++)
	{
		bool selected = m_selection.IsSelected(item);
		const int line_height = GetLineHeight(item);

		if (selected || item == m_currentRow)
		{
			wxRect rowRect(x_start, cur_line_start,
				x_last - x_start, line_height);

			bool renderColumnFocus = false;

			int flags = wxCONTROL_SELECTED;
			if (m_hasFocus)
				flags |= wxCONTROL_FOCUSED;

			// draw keyboard focus rect if applicable
			if (item == m_currentRow && m_hasFocus)
			{

				if (m_useCellFocus && m_currentCol && m_currentColSetByKeyboard)
				{
					renderColumnFocus = true;

					// If there is just a single value, render full-row focus:
					if (!IsList())
					{
						CDataViewTreeNode *node = GetTreeNodeByRow(item);
						if (IsItemSingleValued(node->GetItem()))
							renderColumnFocus = false;
					}
				}

				if (renderColumnFocus)
				{
					wxRect colRect(rowRect);

					for (unsigned int i = col_start; i < col_last; i++)
					{
						CDataViewColumn *col = GetOwner()->GetColumnAt(i);
						if (col->IsHidden())
							continue;

						colRect.width = col->GetWidth();

						if (col == m_currentCol)
						{
							// Draw selection rect left of column
							{
								wxRect clipRect(rowRect);
								clipRect.width = colRect.x;

								wxDCClipper clip(dc, clipRect);
								wxRendererNative::Get().DrawItemSelectionRect
								(
									this,
									dc,
									rowRect,
									flags
								);
							}

							// Draw selection rect right of column
							{
								wxRect clipRect(rowRect);
								clipRect.x = colRect.x + colRect.width;
								clipRect.width = rowRect.width - clipRect.x;

								wxDCClipper clip(dc, clipRect);
								wxRendererNative::Get().DrawItemSelectionRect
								(
									this,
									dc,
									rowRect,
									flags
								);
							}

							// Draw column selection rect
							wxRendererNative::Get().DrawItemSelectionRect
							(
								this,
								dc,
								colRect,
								flags | wxCONTROL_CURRENT | wxCONTROL_CELL
							);

							break;
						}

						colRect.x += colRect.width;
					}
				}
				else // Not using column focus.
				{
					flags |= wxCONTROL_CURRENT | wxCONTROL_FOCUSED;

					// We still need to show the current item if it's not
					// selected.
					if (!selected)
					{
						wxRendererNative::Get().DrawFocusRect
						(
							this,
							dc,
							rowRect,
							flags
						);
					}
					//else: The current item is selected, will be drawn below.
				}
			}

			// draw selection and whole-item focus:
			if (selected && !renderColumnFocus)
			{
				wxRendererNative::Get().DrawItemSelectionRect
				(
					this,
					dc,
					rowRect,
					flags
				);
			}
		}
		cur_line_start += line_height;
	}

#if wxUSE_DRAG_AND_DROP
	wxRect dropItemRect;

	if (m_dropItemInfo.m_hint == DropHint_Inside)
	{
		int rect_y = GetLineStart(m_dropItemInfo.m_row);
		int rect_h = GetLineHeight(m_dropItemInfo.m_row);
		wxRect rect(x_start, rect_y, x_last - x_start, rect_h);

		wxRendererNative::Get().DrawItemSelectionRect(this, dc, rect, wxCONTROL_SELECTED | wxCONTROL_FOCUSED);
	}
#endif // wxUSE_DRAG_AND_DROP

	CDataViewColumn * const
		expander = GetExpanderColumnOrFirstOne(GetOwner());

	// redraw all cells for all rows which must be repainted and all columns
	wxRect cell_rect;
	cell_rect.x = x_start;
	for (unsigned int i = col_start; i < col_last; i++)
	{
		CDataViewColumn *col = GetOwner()->GetColumnAt(i);
		if (col->IsHidden())
			continue;       // skip it!

		wxDataViewRenderer *cell = col->GetRenderer();
		cell_rect.width = col->GetWidth();
		if (cell_rect.width <= 0)
			continue;

		cell_rect.y = first_line_start;
		for (unsigned int item = item_start; item < item_last; item++)
		{
			// get the cell value and set it into the renderer
			CDataViewTreeNode *node = NULL;
			wxDataViewItem dataitem;
			const int line_height = GetLineHeight(item);
			bool hasValue = true;

			if (!IsVirtualList())
			{
				node = GetTreeNodeByRow(item);
				if (node == NULL)
				{
					cell_rect.y += line_height;
					continue;
				}

				dataitem = node->GetItem();

				if (!model->HasValue(dataitem, col->GetModelColumn()))
					hasValue = false;

			}
			else
			{
				dataitem = wxDataViewItem(wxUIntToPtr(item + 1));
			}

			// update cell_rect
			cell_rect.height = line_height;

			bool selected = m_selection.IsSelected(item);

			int state = 0;
			if (m_hasFocus && selected)
				state |= wxDATAVIEW_CELL_SELECTED;

			cell->SetState(state);
			if (hasValue)
				cell->PrepareForItem(model, dataitem, col->GetModelColumn());

			// draw the background
			if (!selected)
				DrawCellBackground(cell, dc, cell_rect);

			// deal with the expander
			int indent = 0;
			if ((!IsList()) && (col == expander))
			{
				// Calculate the indent first
				indent = GetOwner()->GetIndent() * node->GetIndentLevel();

				// Get expander size
				wxSize expSize = wxRendererNative::Get().GetExpanderSize(this);

				// draw expander if needed
				if (node->HasChildren())
				{
					wxRect rect = cell_rect;
					rect.x += indent;
					rect.y += (cell_rect.GetHeight() - expSize.GetHeight()) / 2; // center vertically
					rect.width = expSize.GetWidth();
					rect.height = expSize.GetHeight();

					int flag = 0;
					if (m_underMouse == node)
						flag |= wxCONTROL_CURRENT;
					if (node->IsOpen())
						flag |= wxCONTROL_EXPANDED;

					// ensure that we don't overflow the cell (which might
					// happen if the column is very narrow)
					wxDCClipper clip(dc, cell_rect);

					wxRendererNative::Get().DrawTreeItemButton(this, dc, rect, flag);
				}

				indent += expSize.GetWidth();

				// force the expander column to left-center align
				cell->SetAlignment(wxALIGN_CENTER_VERTICAL);

#if wxUSE_DRAG_AND_DROP
				if (item == m_dropItemInfo.m_row)
				{
					dropItemRect = cell_rect;
					dropItemRect.x += expSize.GetWidth();
					dropItemRect.width -= expSize.GetWidth();
					if (m_dropItemInfo.m_indentLevel >= 0)
					{
						int hintIndent = GetOwner()->GetIndent()*m_dropItemInfo.m_indentLevel;
						dropItemRect.x += hintIndent;
						dropItemRect.width -= hintIndent;
					}
				}
#endif
			}

			wxRect item_rect = cell_rect;
			item_rect.Deflate(PADDING_RIGHTLEFT, 0);

			// account for the tree indent (harmless if we're not indented)
			item_rect.x += indent;
			item_rect.width -= indent;

			if (item_rect.width <= 0)
			{
				cell_rect.y += line_height;
				continue;
			}

			// TODO: it would be much more efficient to create a clipping
			//       region for the entire column being rendered (in the OnPaint
			//       of CDataViewMainWindow) instead of a single clip region for
			//       each cell. However it would mean that each renderer should
			//       respect the given wxRect's top & bottom coords, eventually
			//       violating only the left & right coords - however the user can
			//       make its own renderer and thus we cannot be sure of that.
			wxDCClipper clip(dc, item_rect);

			if (hasValue)
				cell->WXCallRender(item_rect, &dc, state);

			cell_rect.y += line_height;
		}

		cell_rect.x += cell_rect.width;
	}

#if wxUSE_DRAG_AND_DROP
	if (m_dropItemInfo.m_hint == DropHint_Below || m_dropItemInfo.m_hint == DropHint_Above)
	{
		const int insertLineHeight = 2;     // TODO: setup (should be even)

		int rect_y = dropItemRect.y - insertLineHeight / 2;     // top insert
		if (m_dropItemInfo.m_hint == DropHint_Below)
			rect_y += dropItemRect.height;                    // bottom insert

		wxRect rect(dropItemRect.x, rect_y, dropItemRect.width, insertLineHeight);
		wxRendererNative::Get().DrawItemSelectionRect(this, dc, rect, wxCONTROL_SELECTED);
	}
#endif // wxUSE_DRAG_AND_DROP
}


void CDataViewMainWindow::DrawCellBackground(wxDataViewRenderer* cell, wxDC& dc, const wxRect& rect)
{
	wxRect rectBg(rect);

	// don't overlap the horizontal rules
	if (m_owner->HasFlag(wxDV_HORIZ_RULES))
	{
		rectBg.y++;
		rectBg.height--;
	}

	// don't overlap the vertical rules
	if (m_owner->HasFlag(wxDV_VERT_RULES))
	{
		// same note as in OnPaint handler above
		// NB: Vertical rules are drawn in the last pixel of a column so that
		//     they align perfectly with native MSW wxHeaderCtrl as well as for
		//     consistency with MSW native list control. There's no vertical
		//     rule at the most-left side of the control.
		rectBg.width--;
	}

	cell->RenderBackground(&dc, rectBg);
}

void CDataViewMainWindow::OnRenameTimer()
{
	// We have to call this here because changes may just have
	// been made and no screen update taken place.
	if (m_dirty)
	{
		// TODO: use wxTheApp->SafeYieldFor(NULL, wxEVT_CATEGORY_UI) instead
		//       (needs to be tested!)
		wxSafeYield();
	}

	wxDataViewItem item = GetItemByRow(m_currentRow);

	StartEditing(item, m_currentCol);
}

void
CDataViewMainWindow::StartEditing(const wxDataViewItem& item,
	const CDataViewColumn* col)
{
	wxDataViewRenderer* renderer = col->GetRenderer();
	if (!IsCellEditableInMode(item, col, wxDATAVIEW_CELL_EDITABLE))
		return;

	const wxRect itemRect = GetItemRect(item, col);
	if (renderer->StartEditing(item, itemRect))
	{
		renderer->NotifyEditingStarted(item);

		// Save the renderer to be able to finish/cancel editing it later and
		// save the control to be able to detect if we're still editing it.
		m_editorRenderer = renderer;
		m_editorCtrl = renderer->GetEditorCtrl();
	}
}

void CDataViewMainWindow::FinishEditing()
{
	if (m_editorCtrl)
	{
		m_editorRenderer->FinishEditing();
	}
}

void CDataViewHeaderWindow::FinishEditing()
{
	CDataViewMainWindow *win = static_cast<CDataViewMainWindow*>(GetOwner()->GetMainWindow());
	win->FinishEditing();
}

//-----------------------------------------------------------------------------
// Helper class for do operation on the tree node
//-----------------------------------------------------------------------------

namespace
{

	class DoJob
	{
	public:
		DoJob() { }
		virtual ~DoJob() { }

		// The return value control how the tree-walker tranverse the tree
		enum
		{
			DONE,          // Job done, stop traversing and return
			SKIP_SUBTREE,  // Ignore the current node's subtree and continue
			CONTINUE       // Job not done, continue
		};

		virtual int operator() (CDataViewTreeNode * node) = 0;
	};

	bool
		Walker(CDataViewTreeNode * node, DoJob & func, WalkFlags flags = Walk_All)
	{
		wxCHECK_MSG(node, false, "can't walk NULL node");

		switch (func(node))
		{
		case DoJob::DONE:
			return true;
		case DoJob::SKIP_SUBTREE:
			return false;
		case DoJob::CONTINUE:
			break;
		}

		if (node->HasChildren() && (flags != Walk_ExpandedOnly || node->IsOpen()))
		{
			const CDataViewTreeNodes& nodes = node->GetChildNodes();

			for (CDataViewTreeNodes::const_iterator i = nodes.begin();
				i != nodes.end();
				++i)
			{
				if (Walker(*i, func, flags))
					return true;
			}
		}

		return false;
	}

} // anonymous namespace

bool CDataViewMainWindow::ItemAdded(const wxDataViewItem & parent, const wxDataViewItem & item)
{
	if (IsVirtualList())
	{
		CDataViewVirtualListModel *list_model =
			(CDataViewVirtualListModel*)GetModel();
		m_count = list_model->GetCount();
	}
	else
	{
		// specific position (row) is unclear, so clear whole height cache
		ClearRowHeightCache();

		CDataViewTreeNode *parentNode = FindNode(parent);

		if (!parentNode)
			return false;

		parentNode->SetHasChildren(true);

		CDataViewTreeNode *itemNode = new CDataViewTreeNode(parentNode, item);
		itemNode->SetHasChildren(GetModel()->IsContainer(item));

		if (GetSortOrder().IsNone())
		{
			// There's no sorting, so we need to select an insertion position

			wxDataViewItemArray modelSiblings;
			GetModel()->GetChildren(parent, modelSiblings);
			const int modelSiblingsSize = modelSiblings.size();

			int posInModel = modelSiblings.Index(item, /*fromEnd=*/true);
			wxCHECK_MSG(posInModel != wxNOT_FOUND, false, "adding non-existent item?");


			const CDataViewTreeNodes& nodeSiblings = parentNode->GetChildNodes();
			const int nodeSiblingsSize = nodeSiblings.size();

			int nodePos = 0;

			if (posInModel == modelSiblingsSize - 1)
			{
				nodePos = nodeSiblingsSize;
			}
			else if (modelSiblingsSize == nodeSiblingsSize + 1)
			{
				// This is the simple case when our node tree already matches the
				// model and only this one item is missing.
				nodePos = posInModel;
			}
			else
			{
				// It's possible that a larger discrepancy between the model and
				// our realization exists. This can happen e.g. when adding a bunch
				// of items to the model and then calling ItemsAdded() just once
				// afterwards. In this case, we must find the right position by
				// looking at sibling items.

				// append to the end if we won't find a better position:
				nodePos = nodeSiblingsSize;

				for (int nextItemPos = posInModel + 1;
					nextItemPos < modelSiblingsSize;
					nextItemPos++)
				{
					int nextNodePos = parentNode->FindChildByItem(modelSiblings[nextItemPos]);
					if (nextNodePos != wxNOT_FOUND)
					{
						nodePos = nextNodePos;
						break;
					}
				}
			}
			parentNode->ChangeSubTreeCount(+1);
			parentNode->InsertChild(this, itemNode, nodePos);
		}
		else
		{
			// Node list is or will be sorted, so InsertChild do not need insertion position
			parentNode->ChangeSubTreeCount(+1);
			parentNode->InsertChild(this, itemNode, 0);
		}

		InvalidateCount();
	}

	m_selection.OnItemsInserted(GetRowByItem(item), 1);

	GetOwner()->InvalidateColBestWidths();
	UpdateDisplay();

	return true;
}

bool CDataViewMainWindow::ItemDeleted(const wxDataViewItem& parent,
	const wxDataViewItem& item)
{
	if (IsVirtualList())
	{
		CDataViewVirtualListModel *list_model =
			(CDataViewVirtualListModel*)GetModel();
		m_count = list_model->GetCount();

		m_selection.OnItemDelete(GetRowByItem(item));
	}
	else // general case
	{
		CDataViewTreeNode *parentNode = FindNode(parent);

		// Notice that it is possible that the item being deleted is not in the
		// tree at all, for example we could be deleting a never shown (because
		// collapsed) item in a tree model. So it's not an error if we don't know
		// about this item, just return without doing anything then.
		if (!parentNode)
			return true;

		wxCHECK_MSG(parentNode->HasChildren(), false, "parent node doesn't have children?");
		const CDataViewTreeNodes& parentsChildren = parentNode->GetChildNodes();

		// We can't use FindNode() to find 'item', because it was already
		// removed from the model by the time ItemDeleted() is called, so we
		// have to do it manually. We keep track of its position as well for
		// later use.
		int itemPosInNode = 0;
		CDataViewTreeNode *itemNode = NULL;
		for (CDataViewTreeNodes::const_iterator i = parentsChildren.begin();
			i != parentsChildren.end();
			++i, ++itemPosInNode)
		{
			if ((*i)->GetItem() == item)
			{
				itemNode = *i;
				break;
			}
		}

		// If the parent wasn't expanded, it's possible that we didn't have a
		// node corresponding to 'item' and so there's nothing left to do.
		if (!itemNode)
		{
			// If this was the last child to be removed, it's possible the parent
			// node became a leaf. Let's ask the model about it.
			if (parentNode->GetChildNodes().empty())
				parentNode->SetHasChildren(GetModel()->IsContainer(parent));

			return true;
		}

		if (m_rowHeightCache)
			m_rowHeightCache->Remove(GetRowByItem(parent) + itemPosInNode);

		// Delete the item from CDataViewTreeNode representation:
		const int itemsDeleted = 1 + itemNode->GetSubTreeCount();

		parentNode->RemoveChild(itemPosInNode);
		delete itemNode;
		parentNode->ChangeSubTreeCount(-itemsDeleted);

		// Make the row number invalid and get a new valid one when user call GetRowCount
		InvalidateCount();

		// If this was the last child to be removed, it's possible the parent
		// node became a leaf. Let's ask the model about it.
		if (parentNode->GetChildNodes().empty())
		{
			bool isContainer = GetModel()->IsContainer(parent);
			parentNode->SetHasChildren(isContainer);
			if (isContainer)
			{
				// If it's still a container, make sure we show "+" icon for it
				// and not "-" one as there is nothing to collapse any more.
				if (parentNode->IsOpen())
					parentNode->ToggleOpen(this);
			}
		}

		// Update selection by removing 'item' and its entire children tree from the selection.
		if (!m_selection.IsEmpty())
		{
			// we can't call GetRowByItem() on 'item', as it's already deleted, so compute it from
			// the parent ('parentNode') and position in its list of children
			int itemRow;
			if (itemPosInNode == 0)
			{
				// 1st child, row number is that of the parent parentNode + 1
				itemRow = GetRowByItem(parentNode->GetItem()) + 1;
			}
			else
			{
				// row number is that of the sibling above 'item' + its subtree if any + 1
				const CDataViewTreeNode *siblingNode = parentNode->GetChildNodes()[itemPosInNode - 1];

				itemRow = GetRowByItem(siblingNode->GetItem()) +
					siblingNode->GetSubTreeCount() +
					1;
			}

			m_selection.OnItemsDeleted(itemRow, itemsDeleted);
		}
	}

	// Change the current row to the last row if the current exceed the max row number
	if (m_currentRow >= GetRowCount())
		ChangeCurrentRow(m_count - 1);

	GetOwner()->InvalidateColBestWidths();
	UpdateDisplay();

	return true;
}

bool CDataViewMainWindow::DoItemChanged(const wxDataViewItem & item, int view_column)
{
	if (!IsVirtualList())
	{
		if (m_rowHeightCache)
			m_rowHeightCache->Remove(GetRowByItem(item));

		// Move this node to its new correct place after it was updated.
		//
		// In principle, we could skip the call to PutInSortOrder() if the modified
		// column is not the sort column, but in real-world applications it's fully
		// possible and likely that custom compare uses not only the selected model
		// column but also falls back to other values for comparison. To ensure
		// consistency it is better to treat a value change as if it was an item
		// change.
		CDataViewTreeNode* const node = FindNode(item);
		wxCHECK_MSG(node, false, "invalid item");
		node->PutInSortOrder(this);
	}

	CDataViewColumn* column;
	if (view_column == wxNOT_FOUND)
	{
		column = NULL;
		GetOwner()->InvalidateColBestWidths();
	}
	else
	{
		column = m_owner->GetColumn(view_column);
		GetOwner()->InvalidateColBestWidth(view_column);
	}

	// Update the displayed value(s).
	RefreshRow(GetRowByItem(item));

	// Send event
	CDataViewEvent le(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, m_owner, column, item);
	m_owner->ProcessWindowEvent(le);

	return true;
}

bool CDataViewMainWindow::ValueChanged(const wxDataViewItem & item, unsigned int model_column)
{
	int view_column = m_owner->GetModelColumnIndex(model_column);
	if (view_column == wxNOT_FOUND)
		return false;

	return DoItemChanged(item, view_column);
}

bool CDataViewMainWindow::Cleared()
{
	DestroyTree();
	m_selection.Clear();
	m_currentRow = (unsigned)-1;

	ClearRowHeightCache();

	if (GetModel())
	{
		BuildTree(GetModel());
	}
	else
	{
		m_count = 0;
	}

	GetOwner()->InvalidateColBestWidths();
	UpdateDisplay();

	return true;
}

void CDataViewMainWindow::UpdateDisplay()
{
	m_dirty = true;
	m_underMouse = NULL;
}

void CDataViewMainWindow::OnInternalIdle()
{
	wxWindow::OnInternalIdle();

	if (m_dirty)
	{
		RecalculateDisplay();
		m_dirty = false;
	}
}

void CDataViewMainWindow::RecalculateDisplay()
{
	CDataViewModel *model = GetModel();
	if (!model)
	{
		Refresh();
		return;
	}

	int width = GetEndOfLastCol();
	int height = GetLineStart(GetRowCount());

	SetVirtualSize(width, height);
	GetOwner()->SetScrollRate(10, m_lineHeight);
	UpdateColumnSizes();

	Refresh();
}

void CDataViewMainWindow::ScrollWindow(int dx, int dy, const wxRect *rect)
{
	m_underMouse = NULL;

	wxWindow::ScrollWindow(dx, dy, rect);

	if (GetOwner()->m_headerArea)
		GetOwner()->m_headerArea->ScrollWindow(dx, 0);
}

void CDataViewMainWindow::ScrollTo(int rows, int column)
{
	m_underMouse = NULL;

	int x, y;
	m_owner->GetScrollPixelsPerUnit(&x, &y);

	// Take care to not divide by 0 if we're somehow called before scrolling
	// parameters are initialized.
	int sy = y ? GetLineStart(rows) / y : -1;
	int sx = -1;
	if (column != -1 && x)
	{
		wxRect rect = GetClientRect();
		int colnum = 0;
		int x_start, w = 0;
		int xx, yy, xe;
		m_owner->CalcUnscrolledPosition(rect.x, rect.y, &xx, &yy);
		for (x_start = 0; colnum < column; colnum++)
		{
			CDataViewColumn *col = GetOwner()->GetColumnAt(colnum);
			if (col->IsHidden())
				continue;      // skip it!

			w = col->GetWidth();
			x_start += w;
		}

		int x_end = x_start + w;
		xe = xx + rect.width;
		if (x_end > xe)
		{
			sx = (xx + x_end - xe) / x;
		}
		if (x_start < xx)
		{
			sx = x_start / x;
		}
	}
	m_owner->Scroll(sx, sy);
}

int CDataViewMainWindow::GetCountPerPage() const
{
	wxSize size = GetClientSize();
	return size.y / m_lineHeight;
}

wxDataViewItem CDataViewMainWindow::GetTopItem() const
{
	unsigned int item = GetFirstVisibleRow();
	CDataViewTreeNode *node = NULL;
	wxDataViewItem dataitem;

	if (!IsVirtualList())
	{
		node = GetTreeNodeByRow(item);
		if (node == NULL) return wxDataViewItem(0);

		dataitem = node->GetItem();
	}
	else
	{
		dataitem = wxDataViewItem(wxUIntToPtr(item + 1));
	}

	return dataitem;
}

int CDataViewMainWindow::GetEndOfLastCol() const
{
	int width = 0;
	unsigned int i;
	for (i = 0; i < GetOwner()->GetColumnCount(); i++)
	{
		const CDataViewColumn *c =
			const_cast<CDataViewCtrl*>(GetOwner())->GetColumnAt(i);

		if (!c->IsHidden())
			width += c->GetWidth();
	}
	return width;
}

unsigned int CDataViewMainWindow::GetFirstVisibleRow() const
{
	int x = 0;
	int y = 0;
	m_owner->CalcUnscrolledPosition(x, y, &x, &y);

	return GetLineAt(y);
}

unsigned int CDataViewMainWindow::GetLastVisibleRow()
{
	wxSize client_size = GetClientSize();
	// Find row occupying the bottom line of the client area (dimY-1).
	m_owner->CalcUnscrolledPosition(client_size.x, client_size.y - 1,
		&client_size.x, &client_size.y);
	unsigned int row = GetLineAt(client_size.y);

	return wxMin(GetRowCount() - 1, row);
}

unsigned int CDataViewMainWindow::GetLastFullyVisibleRow()
{
	unsigned int row = GetLastVisibleRow();

	int bottom = GetLineStart(row) + GetLineHeight(row);
	m_owner->CalcScrolledPosition(-1, bottom, NULL, &bottom);

	if (bottom > GetClientSize().y)
		return wxMax(0, row - 1);
	else
		return row;
}

unsigned int CDataViewMainWindow::GetRowCount() const
{
	if (m_count == -1)
	{
		CDataViewMainWindow* const
			self = const_cast<CDataViewMainWindow*>(this);
		self->UpdateCount(RecalculateCount());
		self->UpdateDisplay();
	}
	return m_count;
}

void CDataViewMainWindow::ChangeCurrentRow(unsigned int row)
{
	m_currentRow = row;

	// send event
#if wxUSE_ACCESSIBILITY
	wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_FOCUS, m_owner, wxOBJID_CLIENT, m_currentRow + 1);
#endif // wxUSE_ACCESSIBILITY
}

bool CDataViewMainWindow::UnselectAllRows(unsigned int except)
{
	if (!m_selection.IsEmpty())
	{
		for (unsigned i = GetFirstVisibleRow(); i <= GetLastVisibleRow(); i++)
		{
			if (m_selection.IsSelected(i) && i != except)
				RefreshRow(i);
		}

		if (except != (unsigned int)-1)
		{
			const bool wasSelected = m_selection.IsSelected(except);
			ClearSelection();
			if (wasSelected)
			{
				m_selection.SelectItem(except);

				// The special item is still selected.
				return false;
			}
		}
		else
		{
			ClearSelection();
		}
	}

	// There are no selected items left.
	return true;
}

void CDataViewMainWindow::SelectRow(unsigned int row, bool on)
{
	if (m_selection.SelectItem(row, on))
		RefreshRow(row);
}

void CDataViewMainWindow::SelectRows(unsigned int from, unsigned int to)
{
	wxArrayInt changed;
	if (m_selection.SelectRange(from, to, true, &changed))
	{
		for (unsigned i = 0; i < changed.size(); i++)
			RefreshRow(changed[i]);
	}
	else // Selection of too many rows has changed.
	{
		RefreshRows(from, to);
	}
}

void CDataViewMainWindow::Select(const wxArrayInt& aSelections)
{
	for (size_t i = 0; i < aSelections.GetCount(); i++)
	{
		int n = aSelections[i];

		if (m_selection.SelectItem(n))
			RefreshRow(n);
	}
}

void CDataViewMainWindow::ReverseRowSelection(unsigned int row)
{
	m_selection.SelectItem(row, !m_selection.IsSelected(row));
	RefreshRow(row);
}

bool CDataViewMainWindow::IsRowSelected(unsigned int row)
{
	return m_selection.IsSelected(row);
}

void CDataViewMainWindow::SendSelectionChangedEvent(const wxDataViewItem& item)
{
#if wxUSE_ACCESSIBILITY
	wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_SELECTIONWITHIN, m_owner, wxOBJID_CLIENT, wxACC_SELF);
#endif // wxUSE_ACCESSIBILITY

	CDataViewEvent le(wxEVT_DATAVIEW_SELECTION_CHANGED, m_owner, item);
	m_owner->ProcessWindowEvent(le);
}

void CDataViewMainWindow::RefreshRows(unsigned int from, unsigned int to)
{
	wxRect rect = GetLinesRect(from, to);

	m_owner->CalcScrolledPosition(rect.x, rect.y, &rect.x, &rect.y);

	wxSize client_size = GetClientSize();
	wxRect client_rect(0, 0, client_size.x, client_size.y);
	wxRect intersect_rect = client_rect.Intersect(rect);
	if (!intersect_rect.IsEmpty())
		Refresh(true, &intersect_rect);
}

void CDataViewMainWindow::RefreshRowsAfter(unsigned int firstRow)
{
	wxSize client_size = GetClientSize();
	int start = GetLineStart(firstRow);
	m_owner->CalcScrolledPosition(start, 0, &start, NULL);
	if (start > client_size.y) return;

	wxRect rect(0, start, client_size.x, client_size.y - start);

	Refresh(true, &rect);
}

wxRect CDataViewMainWindow::GetLinesRect(unsigned int rowFrom, unsigned int rowTo) const
{
	if (rowFrom > rowTo)
		wxSwap(rowFrom, rowTo);

	wxRect rect;
	rect.x = 0;
	rect.y = GetLineStart(rowFrom);
	// Don't calculate exact width of the row, because GetEndOfLastCol() is
	// expensive to call, and controls with rows not spanning entire width rare.
	// It is more efficient to e.g. repaint empty parts of the window needlessly.
	rect.width = INT_MAX;
	if (rowFrom == rowTo)
		rect.height = GetLineHeight(rowFrom);
	else
		rect.height = GetLineStart(rowTo) - rect.y + GetLineHeight(rowTo);
	return rect;
}

int CDataViewMainWindow::GetLineStart(unsigned int row) const
{
	// check for the easy case first
	if (!m_rowHeightCache || !GetOwner()->HasFlag(wxDV_VARIABLE_LINE_HEIGHT))
		return row * m_lineHeight;

	int start = 0;
	if (m_rowHeightCache->GetLineStart(row, start))
		return start;

	unsigned int r;
	for (r = 0; r < row; r++)
	{
		int height = 0;
		if (!m_rowHeightCache->GetLineHeight(r, height))
		{
			// row height not in cache -> get it from the renderer...
			wxDataViewItem item = GetItemByRow(r);
			if (!item)
				break;

			height = QueryAndCacheLineHeight(r, item);
		}

		start += height;
	}

	return start;
}

int CDataViewMainWindow::GetLineAt(unsigned int y) const
{
	// check for the easy case first
	if (!m_rowHeightCache || !GetOwner()->HasFlag(wxDV_VARIABLE_LINE_HEIGHT))
		return y / m_lineHeight;

	unsigned int row = 0;
	if (m_rowHeightCache->GetLineAt(y, row))
		return row;

	// OnPaint asks GetLineAt for the very last y position and this is always
	// below the last item (--> an invalid item). To prevent iterating over all
	// items, check if y is below the last row.
	// Because this is done very often (for each repaint) its worth to handle
	// this special case separately.
	int height = 0;
	int start = 0;
	unsigned int rowCount = GetRowCount();
	if (rowCount == 0 ||
		(m_rowHeightCache->GetLineInfo(rowCount - 1, start, height) &&
			y >= static_cast<unsigned int>(start + height)))
	{
		return rowCount;
	}

	// sum all item heights until y is reached
	unsigned int yy = 0;
	for (;;)
	{
		height = 0;
		if (!m_rowHeightCache->GetLineHeight(row, height))
		{
			// row height not in cache -> get it from the renderer...
			wxDataViewItem item = GetItemByRow(row);
			if (!item)
			{
				wxASSERT(row >= GetRowCount());
				break;
			}

			height = QueryAndCacheLineHeight(row, item);
		}

		yy += height;
		if (y < yy)
			break;

		row++;
	}
	return row;
}

int CDataViewMainWindow::GetLineHeight(unsigned int row) const
{
	// check for the easy case first
	if (!m_rowHeightCache || !GetOwner()->HasFlag(wxDV_VARIABLE_LINE_HEIGHT))
		return m_lineHeight;

	int height = 0;
	if (m_rowHeightCache->GetLineHeight(row, height))
		return height;

	wxDataViewItem item = GetItemByRow(row);
	if (!item)
		return m_lineHeight;

	height = QueryAndCacheLineHeight(row, item);
	return height;
}

int CDataViewMainWindow::QueryAndCacheLineHeight(unsigned int row, wxDataViewItem item) const
{
	const CDataViewModel *model = GetModel();
	int height = m_lineHeight;
	unsigned int cols = GetOwner()->GetColumnCount();
	unsigned int col;
	for (col = 0; col < cols; col++)
	{
		const CDataViewColumn *column = GetOwner()->GetColumn(col);
		if (column->IsHidden())
			continue;      // skip it!

		if (!model->HasValue(item, col))
			continue;      // skip it!

		wxDataViewRenderer *renderer =
			const_cast<wxDataViewRenderer*>(column->GetRenderer());
		renderer->PrepareForItem(model, item, column->GetModelColumn());

		height = wxMax(height, renderer->GetSize().y);
	}

	// ... and store the height in the cache
	m_rowHeightCache->Put(row, height);

	return height;
}


class RowToTreeNodeJob : public DoJob
{
public:
	// Note that we initialize m_current to -1 because the first node passed to
	// our operator() will be the root node, which doesn't appear in the window
	// and so doesn't count as a real row.
	explicit RowToTreeNodeJob(int row)
		: m_row(row), m_current(-1), m_ret(NULL)
	{
	}

	virtual int operator() (CDataViewTreeNode * node) wxOVERRIDE
	{
		if (m_current == m_row)
		{
			m_ret = node;
			return DoJob::DONE;
		}

		if (node->GetSubTreeCount() + m_current < m_row)
		{
			m_current += node->GetSubTreeCount() + 1;
			return  DoJob::SKIP_SUBTREE;
		}
		else
		{
			// If the current node has only leaf children, we can find the
			// desired node directly. This can speed up finding the node
			// in some cases, and will have a very good effect for list views.
			if (node->HasChildren() &&
				(int)node->GetChildNodes().size() == node->GetSubTreeCount())
			{
				const int index = m_row - m_current - 1;
				m_ret = node->GetChildNodes()[index];
				return DoJob::DONE;
			}

			m_current++;

			return DoJob::CONTINUE;
		}
	}

	CDataViewTreeNode * GetResult() const
	{
		return m_ret;
	}

private:
	const int m_row;
	int m_current;
	CDataViewTreeNode* m_ret;
};

CDataViewTreeNode * CDataViewMainWindow::GetTreeNodeByRow(unsigned int row) const
{
	wxASSERT(!IsVirtualList());

	if (row == (unsigned)-1)
		return NULL;

	RowToTreeNodeJob job(static_cast<int>(row));
	Walker(m_root, job);
	return job.GetResult();
}

wxDataViewItem CDataViewMainWindow::GetItemByRow(unsigned int row) const
{
	wxDataViewItem item;
	if (IsVirtualList())
	{
		if (row < GetRowCount())
			item = wxDataViewItem(wxUIntToPtr(row + 1));
	}
	else
	{
		CDataViewTreeNode *node = GetTreeNodeByRow(row);
		if (node)
			item = node->GetItem();
	}

	return item;
}

bool
CDataViewMainWindow::SendExpanderEvent(wxEventType type,
	const wxDataViewItem& item)
{
#if wxUSE_ACCESSIBILITY
	if (type == wxEVT_DATAVIEW_ITEM_EXPANDED || type == wxEVT_DATAVIEW_ITEM_COLLAPSED)
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_REORDER, m_owner, wxOBJID_CLIENT, wxACC_SELF);
#endif // wxUSE_ACCESSIBILITY

	CDataViewEvent le(type, m_owner, item);
	return !m_owner->ProcessWindowEvent(le) || le.IsAllowed();
}

bool CDataViewMainWindow::IsExpanded(unsigned int row) const
{
	if (IsList())
		return false;

	CDataViewTreeNode * node = GetTreeNodeByRow(row);
	if (!node)
		return false;

	if (!node->HasChildren())
		return false;

	return node->IsOpen();
}

bool CDataViewMainWindow::HasChildren(unsigned int row) const
{
	if (IsList())
		return false;

	CDataViewTreeNode * node = GetTreeNodeByRow(row);
	if (!node)
		return false;

	if (!node->HasChildren())
		return false;

	return true;
}

void CDataViewMainWindow::Expand(unsigned int row, bool expandChildren)
{
	if (IsList())
		return;

	CDataViewTreeNode * node = GetTreeNodeByRow(row);
	if (!node)
		return;

	return DoExpand(node, row, expandChildren);
}

void
CDataViewMainWindow::DoExpand(CDataViewTreeNode* node,
	unsigned int row,
	bool expandChildren)
{
	if (!node->HasChildren())
		return;

	if (!node->IsOpen())
	{
		if (!SendExpanderEvent(wxEVT_DATAVIEW_ITEM_EXPANDING, node->GetItem()))
		{
			// Vetoed by the event handler.
			return;
		}

		if (m_rowHeightCache)
		{
			// Expand makes new rows visible thus we invalidates all following
			// rows in the height cache
			m_rowHeightCache->Remove(row);
		}

		node->ToggleOpen(this);

		// build the children of current node
		if (node->GetChildNodes().empty())
		{
			::BuildTreeHelper(this, GetModel(), node->GetItem(), node);
		}

		const unsigned countNewRows = node->GetSubTreeCount();

		// Shift all stored indices after this row by the number of newly added
		// rows.
		m_selection.OnItemsInserted(row + 1, countNewRows);
		if (m_currentRow > row)
			ChangeCurrentRow(m_currentRow + countNewRows);

		if (m_count != -1)
			m_count += countNewRows;

		// Expanding this item means the previously cached column widths could
		// have become invalid as new items are now visible.
		GetOwner()->InvalidateColBestWidths();

		UpdateDisplay();
		// Send the expanded event
		SendExpanderEvent(wxEVT_DATAVIEW_ITEM_EXPANDED, node->GetItem());
	}

	// Note that we have to expand the children when expanding recursively even
	// when this node itself was already open.
	if (expandChildren)
	{
		const CDataViewTreeNodes& children = node->GetChildNodes();

		for (CDataViewTreeNodes::const_iterator i = children.begin();
			i != children.end();
			++i)
		{
			CDataViewTreeNode* const child = *i;

			// Row currently corresponds to the previous item, so increment it
			// first to correspond to this child.
			DoExpand(child, ++row, true);

			// We don't need +1 here because we'll increment the row during the
			// next loop iteration.
			row += child->GetSubTreeCount();
		}
	}
}

void CDataViewMainWindow::Collapse(unsigned int row)
{
	if (IsList())
		return;

	CDataViewTreeNode *node = GetTreeNodeByRow(row);
	if (!node)
		return;

	if (!node->HasChildren())
		return;

	if (m_rowHeightCache)
	{
		// Collapse hides rows thus we invalidates all following
		// rows in the height cache
		m_rowHeightCache->Remove(row);
	}

	if (node->IsOpen())
	{
		if (!SendExpanderEvent(wxEVT_DATAVIEW_ITEM_COLLAPSING, node->GetItem()))
		{
			// Vetoed by the event handler.
			return;
		}

		const unsigned countDeletedRows = node->GetSubTreeCount();

		if (m_selection.OnItemsDeleted(row + 1, countDeletedRows))
		{
			SendSelectionChangedEvent(GetItemByRow(row));
		}

		node->ToggleOpen(this);

		// Adjust the current row if necessary.
		if (m_currentRow > row)
		{
			// If the current row was among the collapsed items, make the
			// parent itself current.
			if (m_currentRow <= row + countDeletedRows)
				ChangeCurrentRow(row);
			else // Otherwise just update the index.
				ChangeCurrentRow(m_currentRow - countDeletedRows);
		}

		if (m_count != -1)
			m_count -= countDeletedRows;

		GetOwner()->InvalidateColBestWidths();

		UpdateDisplay();
		SendExpanderEvent(wxEVT_DATAVIEW_ITEM_COLLAPSED, node->GetItem());
	}
}

CDataViewTreeNode * CDataViewMainWindow::FindNode(const wxDataViewItem & item)
{
	const CDataViewModel * model = GetModel();
	if (model == NULL)
		return NULL;

	if (!item.IsOk())
		return m_root;

	// Compose the parent-chain for the item we are looking for
	wxVector<wxDataViewItem> parentChain;
	wxDataViewItem it(item);
	while (it.IsOk())
	{
		parentChain.push_back(it);
		it = model->GetParent(it);
	}

	// Find the item along the parent-chain.
	// This algorithm is designed to speed up the node-finding method
	CDataViewTreeNode* node = m_root;
	for (unsigned iter = parentChain.size() - 1; ; --iter)
	{
		if (node->HasChildren())
		{
			if (node->GetChildNodes().empty())
			{
				// Even though the item is a container, it doesn't have any
				// child nodes in the control's representation yet. We have
				// to realize its subtree now.
				::BuildTreeHelper(this, model, node->GetItem(), node);
			}

			const CDataViewTreeNodes& nodes = node->GetChildNodes();
			bool found = false;

			for (unsigned i = 0; i < nodes.size(); ++i)
			{
				CDataViewTreeNode* currentNode = nodes[i];
				if (currentNode->GetItem() == parentChain[iter])
				{
					if (currentNode->GetItem() == item)
						return currentNode;

					node = currentNode;
					found = true;
					break;
				}
			}
			if (!found)
				return NULL;
		}
		else
			return NULL;

		if (!iter)
			break;
	}
	return NULL;
}

void CDataViewMainWindow::HitTest(const wxPoint & point, wxDataViewItem & item,
	CDataViewColumn* &column)
{
	CDataViewColumn *col = NULL;
	unsigned int cols = GetOwner()->GetColumnCount();
	unsigned int colnum = 0;
	int x, y;
	m_owner->CalcUnscrolledPosition(point.x, point.y, &x, &y);
	for (unsigned x_start = 0; colnum < cols; colnum++)
	{
		col = GetOwner()->GetColumnAt(colnum);
		if (col->IsHidden())
			continue;      // skip it!

		unsigned int w = col->GetWidth();
		if (x_start + w >= (unsigned int)x)
			break;

		x_start += w;
	}

	column = col;
	item = GetItemByRow(GetLineAt(y));
}

wxRect CDataViewMainWindow::GetItemRect(const wxDataViewItem & item,
	const CDataViewColumn* column)
{
	int xpos = 0;
	int width = 0;

	unsigned int cols = GetOwner()->GetColumnCount();
	// If column is null the loop will compute the combined width of all columns.
	// Otherwise, it will compute the x position of the column we are looking for.
	for (unsigned int i = 0; i < cols; i++)
	{
		CDataViewColumn* col = GetOwner()->GetColumnAt(i);

		if (col == column)
			break;

		if (col->IsHidden())
			continue;      // skip it!

		xpos += col->GetWidth();
		width += col->GetWidth();
	}

	if (column != 0)
	{
		// If we have a column, we need can get its width directly.
		if (column->IsHidden())
			width = 0;
		else
			width = column->GetWidth();

	}
	else
	{
		// If we have no column, we reset the x position back to zero.
		xpos = 0;
	}

	const int row = GetRowByItem(item, Walk_ExpandedOnly);
	if (row == -1)
	{
		// This means the row is currently not visible at all.
		return wxRect();
	}

	// we have to take an expander column into account and compute its indentation
	// to get the correct x position where the actual text is
	int indent = 0;
	if (!IsList() &&
		(column == 0 || GetExpanderColumnOrFirstOne(GetOwner()) == column))
	{
		CDataViewTreeNode* node = GetTreeNodeByRow(row);
		indent = GetOwner()->GetIndent() * node->GetIndentLevel();
		indent += wxRendererNative::Get().GetExpanderSize(this).GetWidth();
	}

	wxRect itemRect(xpos + indent,
		GetLineStart(row),
		width - indent,
		GetLineHeight(row));

	GetOwner()->CalcScrolledPosition(itemRect.x, itemRect.y,
		&itemRect.x, &itemRect.y);

	// Check if the rectangle is completely outside of the currently visible
	// area and, if so, return an empty rectangle to indicate that the item is
	// not visible.
	if (itemRect.GetBottom() < 0 || itemRect.GetTop() > GetClientSize().y)
	{
		return wxRect();
	}

	return itemRect;
}

int CDataViewMainWindow::RecalculateCount() const
{
	if (IsVirtualList())
	{
		const CDataViewVirtualListModel* list_model =
			static_cast<const CDataViewVirtualListModel*>(GetModel());

		return list_model->GetCount();
	}
	else
	{
		return m_root->GetSubTreeCount();
	}
}

class ItemToRowJob : public DoJob
{
public:
	// As with RowToTreeNodeJob above, we initialize m_current to -1 because
	// the first node passed to our operator() is the root node which is not
	// visible on screen and so we should return 0 for its first child node and
	// not for the root itself.
	ItemToRowJob(const wxDataViewItem& item, wxVector<wxDataViewItem>::reverse_iterator iter)
		: m_item(item), m_iter(iter), m_current(-1)
	{
	}

	// Maybe binary search will help to speed up this process
	virtual int operator() (CDataViewTreeNode * node) wxOVERRIDE
	{
		if (node->GetItem() == m_item)
		{
			return DoJob::DONE;
		}

		// Is this node the next (grand)parent of the item we're looking for?
		if (node->GetItem() == *m_iter)
		{
			// Search for the next (grand)parent now and skip this item itself.
			++m_iter;
			++m_current;
			return DoJob::CONTINUE;
		}
		else
		{
			// Skip this node and all its currently visible children.
			m_current += node->GetSubTreeCount() + 1;
			return DoJob::SKIP_SUBTREE;
		}

	}

	int GetResult() const
	{
		return m_current;
	}

private:
	const wxDataViewItem m_item;
	wxVector<wxDataViewItem>::reverse_iterator m_iter;

	// The row corresponding to the last node seen in our operator().
	int m_current;

};

int
CDataViewMainWindow::GetRowByItem(const wxDataViewItem & item,
	WalkFlags flags) const
{
	const CDataViewModel * model = GetModel();
	if (model == NULL)
		return -1;

	if (IsVirtualList())
	{
		return wxPtrToUInt(item.GetID()) - 1;
	}
	else
	{
		if (!item.IsOk())
			return -1;

		// Compose the parent-chain of the item we are looking for
		wxVector<wxDataViewItem> parentChain;
		wxDataViewItem it(item);
		while (it.IsOk())
		{
			parentChain.push_back(it);
			it = model->GetParent(it);
		}

		// add an 'invalid' item to represent our 'invisible' root node
		parentChain.push_back(wxDataViewItem());

		// the parent chain was created by adding the deepest parent first.
		// so if we want to start at the root node, we have to iterate backwards through the vector
		ItemToRowJob job(item, parentChain.rbegin());
		if (!Walker(m_root, job, flags))
			return -1;

		return job.GetResult();
	}
}

static void BuildTreeHelper(CDataViewMainWindow *window, const CDataViewModel * model,
	const wxDataViewItem & item, CDataViewTreeNode * node)
{
	if (!model->IsContainer(item))
		return;

	wxDataViewItemArray children;
	unsigned int num = model->GetChildren(item, children);

	for (unsigned int index = 0; index < num; index++)
	{
		CDataViewTreeNode *n = new CDataViewTreeNode(node, children[index]);

		if (model->IsContainer(children[index]))
			n->SetHasChildren(true);

		node->InsertChild(window, n, index);
	}

	if (node->IsOpen())
		node->ChangeSubTreeCount(+num);
}

void CDataViewMainWindow::BuildTree(CDataViewModel * model)
{
	DestroyTree();

	if (GetModel()->IsVirtualListModel())
	{
		InvalidateCount();
		return;
	}

	m_root = CDataViewTreeNode::CreateRootNode();

	// First we define a invalid item to fetch the top-level elements
	wxDataViewItem item;
	BuildTreeHelper(this, model, item, m_root);
	InvalidateCount();
}

void CDataViewMainWindow::DestroyTree()
{
	if (!IsVirtualList())
	{
		wxDELETE(m_root);
		m_count = 0;
	}
}

CDataViewColumn*
CDataViewMainWindow::FindColumnForEditing(const wxDataViewItem& item, wxDataViewCellMode mode) const
{
	// Edit the current column editable in 'mode'. If no column is focused
	// (typically because the user has full row selected), try to find the
	// first editable column (this would typically be a checkbox for
	// wxDATAVIEW_CELL_ACTIVATABLE and we don't want to force the user to set
	// focus on the checkbox column; or on the only editable text column).

	CDataViewColumn *candidate = m_currentCol;

	if (candidate && !IsCellEditableInMode(item, candidate, mode))
	{
		if (m_currentColSetByKeyboard)
		{
			// If current column was set by keyboard to something not editable (in
			// 'mode') and the user pressed Space/F2 then do not edit anything
			// because focus is visually on that column and editing
			// something else would be surprising.
			return NULL;
		}
		else
		{
			// But if the current column was set by mouse to something not editable (in
			// 'mode') and the user pressed Space/F2 to edit it, treat the
			// situation as if there was whole-row focus, because that's what is
			// visually indicated and the mouse click could very well be targeted
			// on the row rather than on an individual cell.
			candidate = NULL;
		}
	}

	if (!candidate)
	{
		const unsigned cols = GetOwner()->GetColumnCount();
		for (unsigned i = 0; i < cols; i++)
		{
			CDataViewColumn *c = GetOwner()->GetColumnAt(i);
			if (c->IsHidden())
				continue;

			if (IsCellEditableInMode(item, c, mode))
			{
				candidate = c;
				break;
			}
		}
	}

	// Switch to the first column with value if the current column has no value
	if (candidate && !GetModel()->HasValue(item, candidate->GetModelColumn()))
		candidate = FindFirstColumnWithValue(item);

	if (!candidate)
		return NULL;

	if (!IsCellEditableInMode(item, candidate, mode))
		return NULL;

	return candidate;
}

bool CDataViewMainWindow::IsCellEditableInMode(const wxDataViewItem& item,
	const CDataViewColumn *col,
	wxDataViewCellMode mode) const
{
	if (col->GetRenderer()->GetMode() != mode)
		return false;

	if (!GetModel()->IsEnabled(item, col->GetModelColumn()))
		return false;

	if (!GetModel()->HasValue(item, col->GetModelColumn()))
		return false;

	return true;
}

void CDataViewMainWindow::OnCharHook(wxKeyEvent& event)
{
	if (m_editorCtrl)
	{
		// Handle any keys special for the in-place editor and return without
		// calling Skip() below.
		switch (event.GetKeyCode())
		{
		case WXK_ESCAPE:
			m_editorRenderer->CancelEditing();
			return;

		case WXK_RETURN:
			// Shift-Enter is not special neither.
			if (event.ShiftDown())
				break;
			wxFALLTHROUGH;

		case WXK_TAB:
			// Ctrl/Alt-Tab or Enter could be used for something else, so
			// don't handle them here.
			if (event.HasModifiers())
				break;

			m_editorRenderer->FinishEditing();
			return;
		}
	}
	else if (m_useCellFocus)
	{
		if (event.GetKeyCode() == WXK_TAB && !event.HasModifiers())
		{
			if (event.ShiftDown())
				OnLeftKey(event);
			else
				OnRightKey(event);
			return;
		}
	}

	event.Skip();
}

void CDataViewMainWindow::OnChar(wxKeyEvent &event)
{
	wxWindow * const parent = GetParent();

	// propagate the char event upwards
	wxKeyEvent eventForParent(event);
	eventForParent.SetEventObject(parent);
	if (parent->ProcessWindowEvent(eventForParent))
		return;

	if (parent->HandleAsNavigationKey(event))
		return;

	// no item -> nothing to do
	if (!HasCurrentRow())
	{
		event.Skip();
		return;
	}

	switch (event.GetKeyCode())
	{
	case WXK_RETURN:
		if (event.HasModifiers())
		{
			event.Skip();
			break;
		}
		else
		{
			// Enter activates the item, i.e. sends wxEVT_DATAVIEW_ITEM_ACTIVATED to
			// it. Only if that event is not handled do we activate column renderer (which
			// is normally done by Space) or even inline editing.

			const wxDataViewItem item = GetItemByRow(m_currentRow);

			CDataViewEvent le(wxEVT_DATAVIEW_ITEM_ACTIVATED, m_owner, item);
			if (m_owner->ProcessWindowEvent(le))
				break;
			// else: fall through to WXK_SPACE handling
		}
		wxFALLTHROUGH;

	case WXK_SPACE:
		if (event.HasModifiers())
		{
			event.Skip();
			break;
		}
		else
		{
			// Space toggles activatable items or -- if not activatable --
			// starts inline editing (this is normally done using F2 on
			// Windows, but Space is common everywhere else, so use it too
			// for greater cross-platform compatibility).

			const wxDataViewItem item = GetItemByRow(m_currentRow);

			// Activate the current activatable column. If not column is focused (typically
			// because the user has full row selected), try to find the first activatable
			// column (this would typically be a checkbox and we don't want to force the user
			// to set focus on the checkbox column).
			CDataViewColumn *activatableCol = FindColumnForEditing(item, wxDATAVIEW_CELL_ACTIVATABLE);

			if (activatableCol)
			{
				const unsigned colIdx = activatableCol->GetModelColumn();
				const wxRect cell_rect = GetOwner()->GetItemRect(item, activatableCol);

				wxDataViewRenderer *cell = activatableCol->GetRenderer();
				cell->PrepareForItem(GetModel(), item, colIdx);
				cell->WXActivateCell(cell_rect, GetModel(), item, colIdx, NULL);

				break;
			}
			// else: fall through to WXK_F2 handling
			wxFALLTHROUGH;
		}

	case WXK_F2:
		if (event.HasModifiers())
		{
			event.Skip();
			break;
		}
		else
		{
			if (!m_selection.IsEmpty())
			{
				// Mimic Windows 7 behavior: edit the item that has focus
				// if it is selected and the first selected item if focus
				// is out of selection.
				unsigned sel;
				if (m_selection.IsSelected(m_currentRow))
				{
					sel = m_currentRow;
				}
				else // Focused item is not selected.
				{
					wxSelectionStore::IterationState cookie;
					sel = m_selection.GetFirstSelectedItem(cookie);
				}


				const wxDataViewItem item = GetItemByRow(sel);

				// Edit the current column. If no column is focused
				// (typically because the user has full row selected), try
				// to find the first editable column.
				CDataViewColumn *editableCol = FindColumnForEditing(item, wxDATAVIEW_CELL_EDITABLE);

				if (editableCol)
					GetOwner()->EditItem(item, editableCol);
			}
		}
		break;

	case WXK_UP:
		GoToRelativeRow(event, -1);
		break;

	case WXK_DOWN:
		GoToRelativeRow(event, +1);
		break;

	case '+':
	case WXK_ADD:
		Expand(m_currentRow);
		break;

	case '*':
	case WXK_MULTIPLY:
		if (!IsExpanded(m_currentRow))
		{
			Expand(m_currentRow, true /* recursively */);
			break;
		}
		//else: fall through to Collapse()
		wxFALLTHROUGH;

	case '-':
	case WXK_SUBTRACT:
		Collapse(m_currentRow);
		break;

	case WXK_LEFT:
		OnLeftKey(event);
		break;

	case WXK_RIGHT:
		OnRightKey(event);
		break;

	case WXK_END:
		GoToRelativeRow(event, +(int)GetRowCount());
		break;

	case WXK_HOME:
		GoToRelativeRow(event, -(int)GetRowCount());
		break;

	case WXK_PAGEUP:
		GoToRelativeRow(event, -(GetCountPerPage() - 1));
		break;

	case WXK_PAGEDOWN:
		GoToRelativeRow(event, +(GetCountPerPage() - 1));
		break;

	default:
		event.Skip();
	}
}

void CDataViewMainWindow::GoToRelativeRow(const wxKeyboardState& kbdState, int delta)
{
	// if there is no selection, we cannot move it anywhere
	if (!HasCurrentRow() || IsEmpty())
		return;

	int newRow = (int)m_currentRow + delta;

	// let's keep the new row inside the allowed range
	if (newRow < 0)
		newRow = 0;

	const int rowCount = (int)GetRowCount();
	if (newRow >= rowCount)
		newRow = rowCount - 1;

	GoToRow(kbdState, newRow);
}

void
CDataViewMainWindow::GoToRow(const wxKeyboardState& kbdState,
	unsigned int newCurrent)
{
	unsigned int oldCurrent = m_currentRow;

	if (newCurrent == oldCurrent)
		return;

	// in single selection we just ignore Shift as we can't select several
	// items anyhow
	if (kbdState.ShiftDown() && !IsSingleSel())
	{
		RefreshRow(oldCurrent);

		ChangeCurrentRow(newCurrent);

		// select all the items between the old and the new one
		if (oldCurrent > newCurrent)
		{
			newCurrent = oldCurrent;
			oldCurrent = m_currentRow;
		}

		SelectRows(oldCurrent, newCurrent);

		wxSelectionStore::IterationState cookie;
		const unsigned firstSel = m_selection.GetFirstSelectedItem(cookie);
		if (firstSel != wxSelectionStore::NO_SELECTION)
			SendSelectionChangedEvent(GetItemByRow(firstSel));
	}
	else // !shift
	{
		RefreshRow(oldCurrent);

		// all previously selected items are unselected unless ctrl is held
		if (!kbdState.ControlDown())
			UnselectAllRows();

		ChangeCurrentRow(newCurrent);

		if (!kbdState.ControlDown())
		{
			SelectRow(m_currentRow, true);
			SendSelectionChangedEvent(GetItemByRow(m_currentRow));
		}
		else
			RefreshRow(m_currentRow);
	}

	GetOwner()->EnsureVisibleRowCol(m_currentRow, -1);
}

void CDataViewMainWindow::OnLeftKey(wxKeyEvent& event)
{
	if (IsList())
	{
		TryAdvanceCurrentColumn(NULL, event, /*forward=*/false);
	}
	else
	{
		CDataViewTreeNode* node = GetTreeNodeByRow(m_currentRow);
		if (!node)
			return;

		if (TryAdvanceCurrentColumn(node, event, /*forward=*/false))
			return;

		const bool dontCollapseNodes = event.GetKeyCode() == WXK_TAB;
		if (dontCollapseNodes)
		{
			m_currentCol = NULL;
			// allow focus change
			event.Skip();
			return;
		}

		// Because TryAdvanceCurrentColumn() return false, we are at the first
		// column or using whole-row selection. In this situation, we can use
		// the standard TreeView handling of the left key.
		if (node->HasChildren() && node->IsOpen())
		{
			Collapse(m_currentRow);
		}
		else
		{
			// if the node is already closed, we move the selection to its parent
			CDataViewTreeNode *parent_node = node->GetParent();

			if (parent_node)
			{
				int parent = GetRowByItem(parent_node->GetItem());
				if (parent >= 0)
				{
					GoToRow(event, parent);
				}
			}
		}
	}
}

void CDataViewMainWindow::OnRightKey(wxKeyEvent& event)
{
	if (IsList())
	{
		TryAdvanceCurrentColumn(NULL, event, /*forward=*/true);
	}
	else
	{
		CDataViewTreeNode* node = GetTreeNodeByRow(m_currentRow);
		if (!node)
			return;

		if (node->HasChildren())
		{
			if (!node->IsOpen())
			{
				Expand(m_currentRow);
			}
			else
			{
				// if the node is already open, we move the selection to the first child
				GoToRelativeRow(event, +1);
			}
		}
		else
		{
			TryAdvanceCurrentColumn(node, event, /*forward=*/true);
		}
	}
}

bool CDataViewMainWindow::TryAdvanceCurrentColumn(CDataViewTreeNode *node, wxKeyEvent& event, bool forward)
{
	if (GetOwner()->GetColumnCount() == 0)
		return false;

	if (!m_useCellFocus)
		return false;

	const bool wrapAround = event.GetKeyCode() == WXK_TAB;

	// navigation shouldn't work in nodes with fewer than two columns
	if (node && IsItemSingleValued(node->GetItem()))
		return false;

	if (m_currentCol == NULL || !m_currentColSetByKeyboard)
	{
		if (forward)
		{
			if (node)
			{
				// find first column with value
				m_currentCol = FindFirstColumnWithValue(node->GetItem());
			}
			else
			{
				// in the special "list" case, all columns have values, so just
				// take the first one
				m_currentCol = GetOwner()->GetColumnAt(0);
			}

			m_currentColSetByKeyboard = true;
			RefreshRow(m_currentRow);
			return true;
		}
		else
		{
			if (!wrapAround)
				return false;
		}
	}

	int idx = GetOwner()->GetColumnIndex(m_currentCol);
	const unsigned int cols = GetOwner()->GetColumnCount();
	for (unsigned int i = 0; i < cols; i++)
	{
		idx += (forward ? +1 : -1);
		if (idx >= (int)GetOwner()->GetColumnCount())
		{
			if (!wrapAround)
				return false;

			if (GetCurrentRow() < GetRowCount() - 1)
			{
				// go to the first column of the next row:
				idx = 0;
				GoToRelativeRow(wxKeyboardState()/*dummy*/, +1);
			}
			else
			{
				// allow focus change
				event.Skip();
				return false;
			}
		}
		else if (idx < 0)
		{
			if (!wrapAround)
				return false;

			if (GetCurrentRow() > 0)
			{
				// go to the last column of the previous row:
				idx = (int)GetOwner()->GetColumnCount() - 1;
				GoToRelativeRow(wxKeyboardState()/*dummy*/, -1);
			}
			else
			{
				// allow focus change
				event.Skip();
				return false;
			}
		}
		if (!node || GetModel()->HasValue(node->GetItem(), i))
			break;
	}

	GetOwner()->EnsureVisibleRowCol(m_currentRow, idx);

	if (idx < 1)
	{
		// We are going to the left of the second column. Reset to whole-row
		// focus (which means first column would be edited).
		m_currentCol = NULL;
		RefreshRow(m_currentRow);
		return true;
	}

	m_currentCol = GetOwner()->GetColumnAt(idx);
	m_currentColSetByKeyboard = true;
	RefreshRow(m_currentRow);
	return true;
}

void CDataViewMainWindow::OnMouse(wxMouseEvent &event)
{
	if (event.GetEventType() == wxEVT_MOUSEWHEEL)
	{
		// let the base handle mouse wheel events.
		event.Skip();
		return;
	}

	int x = event.GetX();
	int y = event.GetY();
	m_owner->CalcUnscrolledPosition(x, y, &x, &y);
	CDataViewColumn *col = NULL;

	int xpos = 0;
	unsigned int cols = GetOwner()->GetColumnCount();
	unsigned int i;
	for (i = 0; i < cols; i++)
	{
		CDataViewColumn *c = GetOwner()->GetColumnAt(i);
		if (c->IsHidden())
			continue;      // skip it!

		if (x < xpos + c->GetWidth())
		{
			col = c;
			break;
		}
		xpos += c->GetWidth();
	}

	CDataViewModel* const model = GetModel();

	const unsigned int current = GetLineAt(y);
	const wxDataViewItem item = GetItemByRow(current);

	if (event.ButtonDown())
	{
		// Not skipping button down events would prevent the system from
		// setting focus to this window as most (all?) of them do by default,
		// so skip it to enable default handling.
		event.Skip();

		// Also stop editing if any mouse button is pressed: this is not really
		// necessary for the left button, as it would result in a focus loss
		// that would make the editor close anyhow, but we do need to do it for
		// the other ones and it does no harm to do it for the left one too.
		FinishEditing();
	}

	// Handle right clicking here, before everything else as context menu
	// events should be sent even when we click outside of any item, unlike all
	// the other ones.
	if (event.RightUp())
	{
		CDataViewEvent le(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, m_owner, col, item);
		m_owner->ProcessWindowEvent(le);
		return;
	}

#if wxUSE_DRAG_AND_DROP
	if (event.Dragging() || ((m_dragCount > 0) && event.Leaving()))
	{
		if (m_dragCount == 0)
		{
			// we have to report the raw, physical coords as we want to be
			// able to call HitTest(event.m_pointDrag) from the user code to
			// get the item being dragged
			m_dragStart = event.GetPosition();
		}

		m_dragCount++;
		if ((m_dragCount < 3) && (event.Leaving()))
			m_dragCount = 3;
		else if (m_dragCount != 3)
			return;

		if (event.LeftIsDown())
		{
			m_owner->CalcUnscrolledPosition(m_dragStart.x, m_dragStart.y,
				&m_dragStart.x, &m_dragStart.y);
			unsigned int drag_item_row = GetLineAt(m_dragStart.y);
			if (drag_item_row >= GetRowCount() || m_dragStart.x > GetEndOfLastCol())
				return;

			wxDataViewItem itemDragged = GetItemByRow(drag_item_row);

			// Notify cell about drag
			CDataViewEvent evt(wxEVT_DATAVIEW_ITEM_BEGIN_DRAG, m_owner, itemDragged);
			if (!m_owner->HandleWindowEvent(evt))
				return;

			if (!evt.IsAllowed())
				return;

			wxDataObject *obj = evt.GetDataObject();
			if (!obj)
				return;

			CDataViewDropSource drag(this, drag_item_row);
			drag.SetData(*obj);
			/* wxDragResult res = */ drag.DoDragDrop(evt.GetDragFlags());
			delete obj;
		}
		return;
	}
	else
	{
		m_dragCount = 0;
	}
#endif // wxUSE_DRAG_AND_DROP

	// Check if we clicked outside the item area.
	if ((current >= GetRowCount()) || !col)
	{
		// Follow Windows convention here: clicking either left or right (but
		// not middle) button clears the existing selection.
		if (m_owner && (event.LeftDown() || event.RightDown()))
		{
			if (!m_selection.IsEmpty())
			{
				m_owner->UnselectAll();
				SendSelectionChangedEvent(wxDataViewItem());
			}
		}
		event.Skip();
		return;
	}

	wxDataViewRenderer *cell = col->GetRenderer();
	CDataViewColumn* const
		expander = GetExpanderColumnOrFirstOne(GetOwner());

	// Test whether the mouse is hovering over the expander (a.k.a tree "+"
	// button) and also determine the offset of the real cell start, skipping
	// the indentation and the expander itself.
	bool hoverOverExpander = false;
	int itemOffset = 0;
	if ((!IsList()) && (expander == col))
	{
		CDataViewTreeNode * node = GetTreeNodeByRow(current);

		int indent = node->GetIndentLevel();
		itemOffset = GetOwner()->GetIndent()*indent;
		const int expWidth = wxRendererNative::Get().GetExpanderSize(this).GetWidth();

		if (node->HasChildren())
		{
			// we make the rectangle we are looking in a bit bigger than the actual
			// visual expander so the user can hit that little thing reliably
			wxRect rect(xpos + itemOffset,
				GetLineStart(current) + (GetLineHeight(current) - m_lineHeight) / 2,
				expWidth, m_lineHeight);

			if (rect.Contains(x, y))
			{
				// So the mouse is over the expander
				hoverOverExpander = true;
				if (m_underMouse && m_underMouse != node)
				{
					// wxLogMessage("Undo the row: %d", GetRowByItem(m_underMouse->GetItem()));
					RefreshRow(GetRowByItem(m_underMouse->GetItem()));
				}
				if (m_underMouse != node)
				{
					// wxLogMessage("Do the row: %d", current);
					RefreshRow(current);
				}
				m_underMouse = node;
			}
		}

		// Account for the expander as well, even if this item doesn't have it,
		// its parent does so it still counts for the offset.
		itemOffset += expWidth;
	}
	if (!hoverOverExpander)
	{
		if (m_underMouse != NULL)
		{
			// wxLogMessage("Undo the row: %d", GetRowByItem(m_underMouse->GetItem()));
			RefreshRow(GetRowByItem(m_underMouse->GetItem()));
			m_underMouse = NULL;
		}
	}

	bool simulateClick = false;

	if (event.ButtonDClick())
	{
		m_renameTimer->Stop();
		m_lastOnSame = false;
	}

	bool ignore_other_columns =
		(expander != col) &&
		(!model->HasValue(item, col->GetModelColumn()));

	if (event.LeftDClick())
	{
		if (!hoverOverExpander && (current == m_lineLastClicked))
		{
			CDataViewEvent le(wxEVT_DATAVIEW_ITEM_ACTIVATED, m_owner, col, item);
			if (m_owner->ProcessWindowEvent(le))
			{
				// Item activation was handled from the user code.
				return;
			}
		}

		// Either it was a double click over the expander, or the second click
		// happened on another item than the first one or it was a bona fide
		// double click which was unhandled. In all these cases we continue
		// processing this event as a simple click, e.g. to select the item or
		// activate the renderer.
		simulateClick = true;
	}

	if (event.LeftUp() && !hoverOverExpander)
	{
		if (m_lineSelectSingleOnUp != (unsigned int)-1)
		{
			// select single line
			if (UnselectAllRows(m_lineSelectSingleOnUp))
			{
				SelectRow(m_lineSelectSingleOnUp, true);
			}

			SendSelectionChangedEvent(GetItemByRow(m_lineSelectSingleOnUp));
		}

		// If the user click the expander, we do not do editing even if the column
		// with expander are editable
		if (m_lastOnSame && !ignore_other_columns)
		{
			if ((col == m_currentCol) && (current == m_currentRow) &&
				IsCellEditableInMode(item, col, wxDATAVIEW_CELL_EDITABLE))
			{
				m_renameTimer->Start(100, true);
			}
		}

		m_lastOnSame = false;
		m_lineSelectSingleOnUp = (unsigned int)-1;
	}
	else if (!event.LeftUp())
	{
		// This is necessary, because after a DnD operation in
		// from and to ourself, the up event is swallowed by the
		// DnD code. So on next non-up event (which means here and
		// now) m_lineSelectSingleOnUp should be reset.
		m_lineSelectSingleOnUp = (unsigned int)-1;
	}

	if (event.RightDown())
	{
		m_lineBeforeLastClicked = m_lineLastClicked;
		m_lineLastClicked = current;

		// If the item is already selected, do not update the selection.
		// Multi-selections should not be cleared if a selected item is clicked.
		if (!IsRowSelected(current))
		{
			UnselectAllRows();

			const unsigned oldCurrent = m_currentRow;
			ChangeCurrentRow(current);
			SelectRow(m_currentRow, true);
			RefreshRow(oldCurrent);
			SendSelectionChangedEvent(GetItemByRow(m_currentRow));
		}
	}
	else if (event.MiddleDown())
	{
	}

	if ((event.LeftDown() || simulateClick) && hoverOverExpander)
	{
		CDataViewTreeNode* node = GetTreeNodeByRow(current);

		// hoverOverExpander being true tells us that our node must be
		// valid and have children.
		// So we don't need any extra checks.
		if (node->IsOpen())
			Collapse(current);
		else
			Expand(current);
	}
	else if ((event.LeftDown() || simulateClick) && !hoverOverExpander)
	{
		m_lineBeforeLastClicked = m_lineLastClicked;
		m_lineLastClicked = current;

		unsigned int oldCurrentRow = m_currentRow;
		bool oldWasSelected = IsRowSelected(m_currentRow);

		bool cmdModifierDown = event.CmdDown();
		if (IsSingleSel() || !(cmdModifierDown || event.ShiftDown()))
		{
			if (IsSingleSel() || !IsRowSelected(current))
			{
				ChangeCurrentRow(current);
				if (UnselectAllRows(current))
				{
					SelectRow(m_currentRow, true);
					SendSelectionChangedEvent(GetItemByRow(m_currentRow));
				}
			}
			else // multi sel & current is highlighted & no mod keys
			{
				m_lineSelectSingleOnUp = current;
				ChangeCurrentRow(current); // change focus
			}
		}
		else // multi sel & either ctrl or shift is down
		{
			if (cmdModifierDown)
			{
				ChangeCurrentRow(current);
				ReverseRowSelection(m_currentRow);
				SendSelectionChangedEvent(GetItemByRow(m_currentRow));
			}
			else if (event.ShiftDown())
			{
				ChangeCurrentRow(current);

				unsigned int lineFrom = oldCurrentRow,
					lineTo = current;

				if (lineFrom == static_cast<unsigned>(-1))
				{
					// If we hadn't had any current row before, treat this as a
					// simple click and select the new row only.
					lineFrom = current;
				}

				if (lineTo < lineFrom)
				{
					lineTo = lineFrom;
					lineFrom = m_currentRow;
				}

				SelectRows(lineFrom, lineTo);

				wxSelectionStore::IterationState cookie;
				const unsigned firstSel = m_selection.GetFirstSelectedItem(cookie);
				if (firstSel != wxSelectionStore::NO_SELECTION)
					SendSelectionChangedEvent(GetItemByRow(firstSel));
			}
			else // !ctrl, !shift
			{
				// test in the enclosing if should make it impossible
				wxFAIL_MSG(wxT("how did we get here?"));
			}
		}

		if (m_currentRow != oldCurrentRow)
			RefreshRow(oldCurrentRow);

		CDataViewColumn *oldCurrentCol = m_currentCol;

		// Update selection here...
		m_currentCol = col;
		m_currentColSetByKeyboard = false;

		// This flag is used to decide whether we should start editing the item
		// label. We do it if the user clicks twice (but not double clicks,
		// i.e. simulateClick is false) on the same item but not if the click
		// was used for something else already, e.g. selecting the item (so it
		// must have been already selected) or giving the focus to the control
		// (so it must have had focus already).
		m_lastOnSame = !simulateClick && ((col == oldCurrentCol) &&
			(current == oldCurrentRow)) && oldWasSelected &&
			HasFocus();

		// Call ActivateCell() after everything else as under GTK+
		if (IsCellEditableInMode(item, col, wxDATAVIEW_CELL_ACTIVATABLE))
		{
			// notify cell about click

			wxRect cell_rect(xpos + itemOffset,
				GetLineStart(current),
				col->GetWidth() - itemOffset,
				GetLineHeight(current));

			// Note that PrepareForItem() should be called after GetLineStart()
			// call in cell_rect initialization above as GetLineStart() calls
			// PrepareForItem() for other items from inside it.
			cell->PrepareForItem(model, item, col->GetModelColumn());

			// Report position relative to the cell's custom area, i.e.
			// not the entire space as given by the control but the one
			// used by the renderer after calculation of alignment etc.
			//
			// Notice that this results in negative coordinates when clicking
			// in the upper left corner of a centre-aligned cell which doesn't
			// fill its column entirely so this is somewhat surprising, but we
			// do it like this for compatibility with the native GTK+ version,
			// see #12270.

			// adjust the rectangle ourselves to account for the alignment
			const int align = cell->GetEffectiveAlignment();

			wxRect rectItem = cell_rect;
			const wxSize size = cell->GetSize();
			if (size.x >= 0 && size.x < cell_rect.width)
			{
				if (align & wxALIGN_CENTER_HORIZONTAL)
					rectItem.x += (cell_rect.width - size.x) / 2;
				else if (align & wxALIGN_RIGHT)
					rectItem.x += cell_rect.width - size.x;
				// else: wxALIGN_LEFT is the default
			}

			if (size.y >= 0 && size.y < cell_rect.height)
			{
				if (align & wxALIGN_CENTER_VERTICAL)
					rectItem.y += (cell_rect.height - size.y) / 2;
				else if (align & wxALIGN_BOTTOM)
					rectItem.y += cell_rect.height - size.y;
				// else: wxALIGN_TOP is the default
			}

			wxMouseEvent event2(event);
			event2.m_x -= rectItem.x;
			event2.m_y -= rectItem.y;
			m_owner->CalcUnscrolledPosition(event2.m_x, event2.m_y, &event2.m_x, &event2.m_y);

			/* ignore ret */ cell->WXActivateCell
			(
				cell_rect,
				model,
				item,
				col->GetModelColumn(),
				&event2
			);
		}
	}
}

void CDataViewMainWindow::OnSetFocus(wxFocusEvent &event)
{
	m_hasFocus = true;

	// Make the control usable from keyboard once it gets focus by ensuring
	// that it has a current row, if at all possible.
	if (!HasCurrentRow() && !IsEmpty())
	{
		ChangeCurrentRow(0);
	}

	if (HasCurrentRow())
	{
		Refresh();
	}
#if wxUSE_ACCESSIBILITY
	else
	{
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_FOCUS, m_owner, wxOBJID_CLIENT, wxACC_SELF);
	}
#endif // wxUSE_ACCESSIBILITY

	event.Skip();
}

void CDataViewMainWindow::OnKillFocus(wxFocusEvent &event)
{
	m_hasFocus = false;

	if (HasCurrentRow())
		Refresh();

	event.Skip();
}

void CDataViewMainWindow::OnColumnsCountChanged()
{
	int editableCount = 0;

	const unsigned cols = GetOwner()->GetColumnCount();
	for (unsigned i = 0; i < cols; i++)
	{
		CDataViewColumn *c = GetOwner()->GetColumnAt(i);
		if (c->IsHidden())
			continue;
		if (c->GetRenderer()->GetMode() != wxDATAVIEW_CELL_INERT)
			editableCount++;
	}

	m_useCellFocus = (editableCount > 0);

	UpdateDisplay();
}

void CDataViewMainWindow::UpdateColumnSizes()
{
	int colsCount = GetOwner()->GetColumnCount();
	if (!colsCount)
		return;

	CDataViewCtrl *owner = GetOwner();

	int fullWinWidth = GetClientSize().x;

	// Find the last shown column: we shouldn't bother to resize the columns
	// that are hidden anyhow.
	int lastColIndex = -1;
	CDataViewColumn *lastCol wxDUMMY_INITIALIZE(NULL);
	for (int colIndex = colsCount - 1; colIndex >= 0; --colIndex)
	{
		lastCol = owner->GetColumnAt(colIndex);
		if (!lastCol->IsHidden())
		{
			lastColIndex = colIndex;
			break;
		}
	}

	if (lastColIndex == -1)
	{
		// All columns are hidden.
		return;
	}

	int lastColX = 0;
	for (int colIndex = 0; colIndex < lastColIndex; ++colIndex)
	{
		const CDataViewColumn *c = owner->GetColumnAt(colIndex);

		if (!c->IsHidden())
			lastColX += c->GetWidth();
	}

	int colswidth = lastColX + lastCol->GetWidth();
	if (lastColX < fullWinWidth)
	{
		const int availableWidth = fullWinWidth - lastColX;

		// Never make the column automatically smaller than the last width it
		// was explicitly given nor its minimum width (however we do need to
		// reduce it until this size if it's currently wider, so this
		// comparison needs to be strict).
		if (availableWidth < wxMax(lastCol->GetMinWidth(),
			lastCol->WXGetSpecifiedWidth()))
		{
			return;
		}

		lastCol->WXUpdateWidth(availableWidth);

		// All columns fit on screen, so we don't need horizontal scrolling.
		// To prevent flickering scrollbar when resizing the window to be
		// narrower, force-set the virtual width to 0 here. It will eventually
		// be corrected at idle time.
		SetVirtualSize(0, m_virtualSize.y);

		RefreshRect(wxRect(lastColX, 0, availableWidth, GetSize().y));
	}
	else
	{
		// else: don't bother, the columns won't fit anyway
		SetVirtualSize(colswidth, m_virtualSize.y);
	}
}

//-----------------------------------------------------------------------------
// CDataViewCtrl
//-----------------------------------------------------------------------------

wxIMPLEMENT_DYNAMIC_CLASS(CDataViewCtrl, CDataViewCtrlBase);
wxBEGIN_EVENT_TABLE(CDataViewCtrl, CDataViewCtrlBase)
EVT_SIZE(CDataViewCtrl::OnSize)
EVT_DPI_CHANGED(CDataViewCtrl::OnDPIChanged)
wxEND_EVENT_TABLE()

CDataViewCtrl::~CDataViewCtrl()
{
	if (m_notifier)
		GetModel()->RemoveNotifier(m_notifier);

	DoClearColumns();

#if wxUSE_ACCESSIBILITY
	SetAccessible(NULL);
	wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_DESTROY, this, wxOBJID_CLIENT, wxACC_SELF);
#endif // wxUSE_ACCESSIBILITY
}

void CDataViewCtrl::Init()
{
	m_notifier = NULL;

	m_headerArea = NULL;
	m_clientArea = NULL;

	m_colsDirty = false;

	m_allowMultiColumnSort = false;
}

bool CDataViewCtrl::Create(wxWindow *parent,
	wxWindowID id,
	const wxPoint& pos,
	const wxSize& size,
	long style,
	const wxValidator& validator,
	const wxString& name)
{
	//    if ( (style & wxBORDER_MASK) == 0)
	//        style |= wxBORDER_SUNKEN;

	Init();

	if (!wxControl::Create(parent, id, pos, size,
		style | wxScrolledWindowStyle, validator, name))
		return false;

	SetInitialSize(size);

#ifdef __WXMAC__
	MacSetClipChildren(true);
#endif

	m_clientArea = new CDataViewMainWindow(this, wxID_ANY);

	// We use the cursor keys for moving the selection, not scrolling, so call
	// this method to ensure wxScrollHelperEvtHandler doesn't catch all
	// keyboard events forwarded to us from wxListMainWindow.
	DisableKeyboardScrolling();

	if (HasFlag(wxDV_NO_HEADER))
		m_headerArea = NULL;
	else
		m_headerArea = new CDataViewHeaderWindow(this);

	SetTargetWindow(m_clientArea);

	wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
	if (m_headerArea)
		sizer->Add(m_headerArea, 0, wxGROW);
	sizer->Add(m_clientArea, 1, wxGROW);
	SetSizer(sizer);

	EnableSystemThemeByDefault();

#if wxUSE_ACCESSIBILITY
	wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_CREATE, this, wxOBJID_CLIENT, wxACC_SELF);
#endif // wxUSE_ACCESSIBILITY

	return true;
}

wxBorder CDataViewCtrl::GetDefaultBorder() const
{
	return wxBORDER_THEME;
}

wxHeaderCtrl* CDataViewCtrl::GenericGetHeader() const
{
	return m_headerArea;
}

#ifdef __WXMSW__
WXLRESULT CDataViewCtrl::MSWWindowProc(WXUINT nMsg,
	WXWPARAM wParam,
	WXLPARAM lParam)
{
	WXLRESULT rc = CDataViewCtrlBase::MSWWindowProc(nMsg, wParam, lParam);

	// we need to process arrows ourselves for scrolling
	if (nMsg == WM_GETDLGCODE)
	{
		rc |= DLGC_WANTARROWS;
	}

	return rc;
}
#endif

wxSize CDataViewCtrl::GetSizeAvailableForScrollTarget(const wxSize& size)
{
	wxSize newsize = size;
	if (!HasFlag(wxDV_NO_HEADER) && (m_headerArea))
		newsize.y -= m_headerArea->GetSize().y;

	return newsize;
}

void CDataViewCtrl::OnSize(wxSizeEvent &WXUNUSED(event))
{
	// We need to override OnSize so that our scrolled
	// window a) does call Layout() to use sizers for
	// positioning the controls but b) does not query
	// the sizer for their size and use that for setting
	// the scrollable area as set that ourselves by
	// calling SetScrollbar() further down.

	Layout();

	// Update the last column size to take all the available space. Note that
	// this must be done after calling Layout() to update m_clientArea size.
	if (m_clientArea)
		m_clientArea->UpdateColumnSizes();

	AdjustScrollbars();

	// We must redraw the headers if their height changed. Normally this
	// shouldn't happen as the control shouldn't let itself be resized beneath
	// its minimal height but avoid the display artefacts that appear if it
	// does happen, e.g. because there is really not enough vertical space.
	if (!HasFlag(wxDV_NO_HEADER) && m_headerArea &&
		m_headerArea->GetSize().y <= m_headerArea->GetBestSize().y)
	{
		m_headerArea->Refresh();
	}
}

void CDataViewCtrl::OnDPIChanged(wxDPIChangedEvent& event)
{
	if (m_clientArea)
	{
		m_clientArea->ClearRowHeightCache();
		m_clientArea->SetRowHeight(m_clientArea->GetDefaultRowHeight());
	}

	for (unsigned i = 0; i < m_cols.size(); ++i)
	{
		int minWidth = m_cols[i]->GetMinWidth();
		if (minWidth > 0)
			minWidth = minWidth * event.GetNewDPI().x / event.GetOldDPI().x;
		m_cols[i]->SetMinWidth(minWidth);

		int width = m_cols[i]->WXGetSpecifiedWidth();
		if (width > 0)
			width = width * event.GetNewDPI().x / event.GetOldDPI().x;
		m_cols[i]->SetWidth(width);
	}
}

void CDataViewCtrl::SetFocus()
{
	if (m_clientArea)
		m_clientArea->SetFocus();
}

bool CDataViewCtrl::SetFont(const wxFont & font)
{
	if (!wxControl::SetFont(font))
		return false;

	if (m_headerArea)
		m_headerArea->SetFont(font);

	if (m_clientArea)
	{
		m_clientArea->SetFont(font);
		m_clientArea->SetRowHeight(m_clientArea->GetDefaultRowHeight());
	}

	if (m_headerArea || m_clientArea)
	{
		InvalidateColBestWidths();
		Layout();
	}

	return true;
}

#if wxUSE_ACCESSIBILITY
bool CDataViewCtrl::Show(bool show)
{
	bool changed = wxControl::Show(show);
	if (changed)
	{
		wxAccessible::NotifyEvent(show ? wxACC_EVENT_OBJECT_SHOW : wxACC_EVENT_OBJECT_HIDE,
			this, wxOBJID_CLIENT, wxACC_SELF);
	}

	return changed;
}

void CDataViewCtrl::SetName(const wxString &name)
{
	wxControl::SetName(name);
	wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_NAMECHANGE, this, wxOBJID_CLIENT, wxACC_SELF);
}

bool CDataViewCtrl::Reparent(wxWindowBase *newParent)
{
	bool changed = wxControl::Reparent(newParent);
	if (changed)
	{
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_PARENTCHANGE, this, wxOBJID_CLIENT, wxACC_SELF);
	}

	return changed;
}
#endif // wxUSE_ACCESIBILITY

bool CDataViewCtrl::Enable(bool enable)
{
	bool changed = wxControl::Enable(enable);
	if (changed)
	{
#if wxUSE_ACCESSIBILITY
		wxAccessible::NotifyEvent(wxACC_EVENT_OBJECT_STATECHANGE, this, wxOBJID_CLIENT, wxACC_SELF);
#endif // wxUSE_ACCESIBILITY
		Refresh();
	}

	return changed;
}

bool CDataViewCtrl::AssociateModel(CDataViewModel *model)
{
	if (!CDataViewCtrlBase::AssociateModel(model))
		return false;

	if (model)
	{
		m_notifier = new CGenericDataViewModelNotifier(m_clientArea);
		model->AddNotifier(m_notifier);
	}
	else
	{
		// Our previous notifier has either been already deleted when the
		// previous model was DecRef()'d in the base class AssociateModel() or
		// is not associated with us any more because if the model is still
		// alive, it's not used by this control.
		m_notifier = NULL;
	}

	m_clientArea->DestroyTree();

	if (model)
	{
		m_clientArea->BuildTree(model);
	}

	m_clientArea->UpdateDisplay();

	return true;
}

#if wxUSE_DRAG_AND_DROP

bool CDataViewCtrl::EnableDragSource(const wxDataFormat &format)
{
	return m_clientArea->EnableDragSource(format);
}

bool CDataViewCtrl::EnableDropTarget(const wxDataFormat &format)
{
	return m_clientArea->EnableDropTarget(format);
}

#endif // wxUSE_DRAG_AND_DROP

bool CDataViewCtrl::AppendColumn(CDataViewColumn *col)
{
	if (!CDataViewCtrlBase::AppendColumn(col))
		return false;

	m_cols.push_back(col);
	m_colsBestWidths.push_back(CachedColWidthInfo());
	OnColumnsCountChanged();
	return true;
}

bool CDataViewCtrl::PrependColumn(CDataViewColumn *col)
{
	if (!CDataViewCtrlBase::PrependColumn(col))
		return false;

	m_cols.insert(m_cols.begin(), col);
	m_colsBestWidths.insert(m_colsBestWidths.begin(), CachedColWidthInfo());
	OnColumnsCountChanged();
	return true;
}

bool CDataViewCtrl::InsertColumn(unsigned int pos, CDataViewColumn *col)
{
	if (!CDataViewCtrlBase::InsertColumn(pos, col))
		return false;

	m_cols.insert(m_cols.begin() + pos, col);
	m_colsBestWidths.insert(m_colsBestWidths.begin() + pos, CachedColWidthInfo());
	OnColumnsCountChanged();
	return true;
}

void CDataViewCtrl::OnColumnResized()
{
	m_clientArea->UpdateDisplay();
}

void CDataViewCtrl::OnColumnWidthChange(unsigned int idx)
{
	InvalidateColBestWidth(idx);

	OnColumnChange(idx);
}

void CDataViewCtrl::OnColumnChange(unsigned int idx)
{
	if (m_headerArea)
		m_headerArea->UpdateColumn(idx);

	m_clientArea->UpdateDisplay();
}

void CDataViewCtrl::OnColumnsCountChanged()
{
	if (m_headerArea)
		m_headerArea->SetColumnCount(GetColumnCount());

	m_clientArea->OnColumnsCountChanged();
}

void CDataViewCtrl::DoSetExpanderColumn()
{
	CDataViewColumn* column = GetExpanderColumn();
	if (column)
	{
		int index = GetColumnIndex(column);
		if (index != wxNOT_FOUND)
			InvalidateColBestWidth(index);
	}

	m_clientArea->UpdateDisplay();
}

void CDataViewCtrl::DoSetIndent()
{
	m_clientArea->UpdateDisplay();
}

unsigned int CDataViewCtrl::GetColumnCount() const
{
	return m_cols.size();
}

bool CDataViewCtrl::SetRowHeight(int lineHeight)
{
	if (!m_clientArea)
		return false;

	m_clientArea->SetRowHeight(lineHeight);

	return true;
}

CDataViewColumn* CDataViewCtrl::GetColumn(unsigned int idx) const
{
	return m_cols[idx];
}

CDataViewColumn *CDataViewCtrl::GetColumnAt(unsigned int pos) const
{
	// columns can't be reordered if there is no header window which allows
	// to do this
	const unsigned idx = m_headerArea ? m_headerArea->GetColumnsOrder()[pos]
		: pos;

	return GetColumn(idx);
}

int CDataViewCtrl::GetColumnIndex(const CDataViewColumn *column) const
{
	const unsigned count = m_cols.size();
	for (unsigned n = 0; n < count; n++)
	{
		if (m_cols[n] == column)
			return n;
	}

	return wxNOT_FOUND;
}

int CDataViewCtrl::GetModelColumnIndex(unsigned int model_column) const
{
	const int count = GetColumnCount();
	for (int index = 0; index < count; index++)
	{
		CDataViewColumn* column = GetColumn(index);
		if (column->GetModelColumn() == model_column)
			return index;
	}
	return wxNOT_FOUND;
}

class CDataViewMaxWidthCalculator : public wxMaxWidthCalculatorBase
{
public:
	CDataViewMaxWidthCalculator(const CDataViewCtrl *dvc,
		CDataViewMainWindow *clientArea,
		wxDataViewRenderer *renderer,
		const CDataViewModel *model,
		size_t model_column,
		int expanderSize)
		: wxMaxWidthCalculatorBase(model_column),
		m_dvc(dvc),
		m_clientArea(clientArea),
		m_renderer(renderer),
		m_model(model),
		m_expanderSize(expanderSize)
	{
		int index = dvc->GetModelColumnIndex(model_column);
		CDataViewColumn* column = index == wxNOT_FOUND ? NULL : dvc->GetColumn(index);
		m_isExpanderCol =
			!clientArea->IsList() &&
			(column == 0 ||
				GetExpanderColumnOrFirstOne(const_cast<CDataViewCtrl*>(dvc)) == column);
	}

	virtual void UpdateWithRow(int row) wxOVERRIDE
	{
		int width = 0;
		wxDataViewItem item;

		if (m_isExpanderCol)
		{
			CDataViewTreeNode *node = m_clientArea->GetTreeNodeByRow(row);
			item = node->GetItem();
			width = m_dvc->GetIndent() * node->GetIndentLevel() + m_expanderSize;
		}
		else
		{
			item = m_clientArea->GetItemByRow(row);
		}

		if (m_model->HasValue(item, GetColumn()))
		{
			m_renderer->PrepareForItem(m_model, item, GetColumn());
			width += m_renderer->GetSize().x;
		}

		UpdateWithWidth(width);
	}

private:
	const CDataViewCtrl *m_dvc;
	CDataViewMainWindow *m_clientArea;
	wxDataViewRenderer *m_renderer;
	const CDataViewModel *m_model;
	bool m_isExpanderCol;
	int m_expanderSize;
};


unsigned int CDataViewCtrl::GetBestColumnWidth(int idx) const
{
	if (m_colsBestWidths[idx].width != 0)
		return m_colsBestWidths[idx].width;

	const int count = m_clientArea->GetRowCount();
	CDataViewColumn *column = GetColumn(idx);
	wxDataViewRenderer *renderer =
		const_cast<wxDataViewRenderer*>(column->GetRenderer());

	CDataViewMaxWidthCalculator calculator(this, m_clientArea, renderer,
		GetModel(), column->GetModelColumn(),
		m_clientArea->GetRowHeight());

	calculator.UpdateWithWidth(column->GetMinWidth());

	if (m_headerArea)
		calculator.UpdateWithWidth(m_headerArea->GetColumnTitleWidth(*column));

	const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
	calculator.ComputeBestColumnWidth(count,
		m_clientArea->GetLineAt(origin.y),
		m_clientArea->GetLineAt(origin.y + GetClientSize().y));

	int max_width = calculator.GetMaxWidth();
	if (max_width > 0)
		max_width += 2 * PADDING_RIGHTLEFT;

	const_cast<CDataViewCtrl*>(this)->m_colsBestWidths[idx].width = max_width;
	return max_width;
}

void CDataViewCtrl::ColumnMoved(CDataViewColumn *col, unsigned int new_pos)
{
	// do _not_ reorder m_cols elements here, they should always be in the
	// order in which columns were added, we only display the columns in
	// different order
	m_clientArea->UpdateDisplay();

	CDataViewEvent event(wxEVT_DATAVIEW_COLUMN_REORDERED, this, col);
	event.SetColumn(new_pos);
	ProcessWindowEvent(event);
}

bool CDataViewCtrl::DeleteColumn(CDataViewColumn *column)
{
	const int idx = GetColumnIndex(column);
	if (idx == wxNOT_FOUND)
		return false;

	m_colsBestWidths.erase(m_colsBestWidths.begin() + idx);
	m_cols.erase(m_cols.begin() + idx);

	if (m_clientArea->GetCurrentColumn() == column)
		m_clientArea->ClearCurrentColumn();

	OnColumnsCountChanged();

	return true;
}

void CDataViewCtrl::DoClearColumns()
{
	typedef wxVector<CDataViewColumn*>::const_iterator citer;
	for (citer it = m_cols.begin(); it != m_cols.end(); ++it)
		delete *it;
}

bool CDataViewCtrl::ClearColumns()
{
	SetExpanderColumn(NULL);

	DoClearColumns();

	m_cols.clear();
	m_sortingColumnIdxs.clear();
	m_colsBestWidths.clear();

	m_clientArea->ClearCurrentColumn();

	OnColumnsCountChanged();

	return true;
}

void CDataViewCtrl::InvalidateColBestWidth(int idx)
{
	m_colsBestWidths[idx].width = 0;
	m_colsBestWidths[idx].dirty = true;
	m_colsDirty = true;
}

void CDataViewCtrl::InvalidateColBestWidths()
{
	// mark all columns as dirty:
	m_colsBestWidths.clear();
	m_colsBestWidths.resize(m_cols.size());
	m_colsDirty = true;
}

void CDataViewCtrl::UpdateColWidths()
{
	m_colsDirty = false;

	if (!m_headerArea)
		return;

	const unsigned len = m_colsBestWidths.size();
	for (unsigned i = 0; i < len; i++)
	{
		// Note that we have to have an explicit 'dirty' flag here instead of
		// checking if the width==0, as is done in GetBestColumnWidth().
		//
		// Testing width==0 wouldn't work correctly if some code called
		// GetWidth() after col. width invalidation but before
		// CDataViewCtrl::UpdateColWidths() was called at idle time. This
		// would result in the header's column width getting out of sync with
		// the control itself.
		if (m_colsBestWidths[i].dirty)
		{
			m_headerArea->UpdateColumn(i);
			m_colsBestWidths[i].dirty = false;
		}
	}
}

void CDataViewCtrl::OnInternalIdle()
{
	CDataViewCtrlBase::OnInternalIdle();

	if (m_colsDirty)
		UpdateColWidths();
}

int CDataViewCtrl::GetColumnPosition(const CDataViewColumn *column) const
{
	unsigned int len = GetColumnCount();
	for (unsigned int i = 0; i < len; i++)
	{
		CDataViewColumn * col = GetColumnAt(i);
		if (column == col)
			return i;
	}

	return wxNOT_FOUND;
}

CDataViewColumn *CDataViewCtrl::GetSortingColumn() const
{
	if (m_sortingColumnIdxs.empty())
		return NULL;

	return GetColumn(m_sortingColumnIdxs.front());
}

wxVector<CDataViewColumn *> CDataViewCtrl::GetSortingColumns() const
{
	wxVector<CDataViewColumn *> out;

	for (wxVector<int>::const_iterator it = m_sortingColumnIdxs.begin(),
		end = m_sortingColumnIdxs.end();
		it != end;
		++it)
	{
		out.push_back(GetColumn(*it));
	}

	return out;
}

wxDataViewItem CDataViewCtrl::DoGetCurrentItem() const
{
	return GetItemByRow(m_clientArea->GetCurrentRow());
}

void CDataViewCtrl::DoSetCurrentItem(const wxDataViewItem& item)
{
	const int row = m_clientArea->GetRowByItem(item);

	const unsigned oldCurrent = m_clientArea->GetCurrentRow();
	if (static_cast<unsigned>(row) != oldCurrent)
	{
		m_clientArea->ChangeCurrentRow(row);
		m_clientArea->RefreshRow(oldCurrent);
		m_clientArea->RefreshRow(row);
	}
}

CDataViewColumn *CDataViewCtrl::GetCurrentColumn() const
{
	return m_clientArea->GetCurrentColumn();
}

int CDataViewCtrl::GetSelectedItemsCount() const
{
	return m_clientArea->GetSelections().GetSelectedCount();
}

wxDataViewItem CDataViewCtrl::GetTopItem() const
{
	return m_clientArea->GetTopItem();
}

int CDataViewCtrl::GetCountPerPage() const
{
	return m_clientArea->GetCountPerPage();
}

int CDataViewCtrl::GetSelections(wxDataViewItemArray & sel) const
{
	sel.Empty();
	const wxSelectionStore& selections = m_clientArea->GetSelections();

	wxSelectionStore::IterationState cookie;
	for (unsigned row = selections.GetFirstSelectedItem(cookie);
		row != wxSelectionStore::NO_SELECTION;
		row = selections.GetNextSelectedItem(cookie))
	{
		wxDataViewItem item = m_clientArea->GetItemByRow(row);
		if (item.IsOk())
		{
			sel.Add(item);
		}
		else
		{
			wxFAIL_MSG("invalid item in selection - bad internal state");
		}
	}

	return sel.size();
}

void CDataViewCtrl::SetSelections(const wxDataViewItemArray & sel)
{
	m_clientArea->ClearSelection();

	if (sel.empty())
		return;

	wxDataViewItem last_parent;

	for (size_t i = 0; i < sel.size(); i++)
	{
		wxDataViewItem item = sel[i];
		wxDataViewItem parent = GetModel()->GetParent(item);
		if (parent)
		{
			if (parent != last_parent)
				ExpandAncestors(item);
		}

		last_parent = parent;
		int row = m_clientArea->GetRowByItem(item);
		if (row >= 0)
			m_clientArea->SelectRow(static_cast<unsigned int>(row), true);
	}

	// Also make the last item as current item
	DoSetCurrentItem(sel.Last());
}

void CDataViewCtrl::Select(const wxDataViewItem & item)
{
	ExpandAncestors(item);

	int row = m_clientArea->GetRowByItem(item);
	if (row >= 0)
	{
		// Unselect all rows before select another in the single select mode
		if (m_clientArea->IsSingleSel())
			m_clientArea->UnselectAllRows();

		m_clientArea->SelectRow(row, true);

		// Also set focus to the selected item
		m_clientArea->ChangeCurrentRow(row);
	}
}

void CDataViewCtrl::Unselect(const wxDataViewItem & item)
{
	int row = m_clientArea->GetRowByItem(item);
	if (row >= 0)
		m_clientArea->SelectRow(row, false);
}

bool CDataViewCtrl::IsSelected(const wxDataViewItem & item) const
{
	int row = m_clientArea->GetRowByItem(item);
	if (row >= 0)
	{
		return m_clientArea->IsRowSelected(row);
	}
	return false;
}

bool CDataViewCtrl::SetHeaderAttr(const wxItemAttr& attr)
{
	if (!m_headerArea)
		return false;

	// Call all functions unconditionally to reset the previously set
	// attributes, if any.
	m_headerArea->SetForegroundColour(attr.GetTextColour());
	m_headerArea->SetBackgroundColour(attr.GetBackgroundColour());
	m_headerArea->SetFont(attr.GetFont());

	// If the font has changed, the size of the header might need to be
	// updated.
	Layout();

	return true;
}

bool CDataViewCtrl::SetAlternateRowColour(const wxColour& colour)
{
	m_alternateRowColour = colour;
	return true;
}

void CDataViewCtrl::SelectAll()
{
	m_clientArea->SelectAllRows();
}

void CDataViewCtrl::UnselectAll()
{
	m_clientArea->UnselectAllRows();
}

void CDataViewCtrl::EnsureVisibleRowCol(int row, int column)
{
	if (row < 0)
		row = 0;
	if (row > (int) m_clientArea->GetRowCount())
		row = m_clientArea->GetRowCount();

	int first = m_clientArea->GetFirstVisibleRow();
	int last = m_clientArea->GetLastFullyVisibleRow();
	if (row < first)
		m_clientArea->ScrollTo(row, column);
	else if (row > last)
		m_clientArea->ScrollTo(row - last + first, column);
	else
		m_clientArea->ScrollTo(first, column);
}

void CDataViewCtrl::EnsureVisible(const wxDataViewItem & item, const CDataViewColumn * column)
{
	ExpandAncestors(item);

	m_clientArea->RecalculateDisplay();

	int row = m_clientArea->GetRowByItem(item);
	if (row >= 0)
	{
		if (column == NULL)
			EnsureVisibleRowCol(row, -1);
		else
			EnsureVisibleRowCol(row, GetColumnIndex(column));
	}

}

void CDataViewCtrl::HitTest(const wxPoint & point, wxDataViewItem & item,
	CDataViewColumn* &column) const
{
	// Convert from CDataViewCtrl coordinates to CDataViewMainWindow coordinates.
	// (They can be different due to the presence of the header.).
	const wxPoint clientPt = m_clientArea->ScreenToClient(ClientToScreen(point));
	m_clientArea->HitTest(clientPt, item, column);
}

wxRect CDataViewCtrl::GetItemRect(const wxDataViewItem & item,
	const CDataViewColumn* column) const
{
	// Convert position from the main window coordinates to the control coordinates.
	// (They can be different due to the presence of the header.).
	wxRect r = m_clientArea->GetItemRect(item, column);
	if (r.width || r.height)
	{
		const wxPoint ctrlPos = ScreenToClient(m_clientArea->ClientToScreen(r.GetPosition()));
		r.SetPosition(ctrlPos);
	}
	return r;
}

wxDataViewItem CDataViewCtrl::GetItemByRow(unsigned int row) const
{
	return m_clientArea->GetItemByRow(row);
}

int CDataViewCtrl::GetRowByItem(const wxDataViewItem & item) const
{
	return m_clientArea->GetRowByItem(item);
}

void CDataViewCtrl::DoExpand(const wxDataViewItem & item, bool expandChildren)
{
	int row = m_clientArea->GetRowByItem(item);
	if (row != -1)
		m_clientArea->Expand(row, expandChildren);
}

void CDataViewCtrl::Collapse(const wxDataViewItem & item)
{
	int row = m_clientArea->GetRowByItem(item);
	if (row != -1)
		m_clientArea->Collapse(row);
}

bool CDataViewCtrl::IsExpanded(const wxDataViewItem & item) const
{
	int row = m_clientArea->GetRowByItem(item);
	if (row != -1)
		return m_clientArea->IsExpanded(row);
	return false;
}

void CDataViewCtrl::EditItem(const wxDataViewItem& item, const CDataViewColumn *column)
{
	wxCHECK_RET(item.IsOk(), "invalid item");
	wxCHECK_RET(column, "no column provided");

	m_clientArea->StartEditing(item, column);
}

void CDataViewCtrl::ResetAllSortColumns()
{
	// Must make copy, because unsorting will remove it from original vector
	wxVector<int> const copy(m_sortingColumnIdxs);
	for (wxVector<int>::const_iterator it = copy.begin(),
		end = copy.end();
		it != end;
		++it)
	{
		GetColumn(*it)->UnsetAsSortKey();
	}

	wxASSERT(m_sortingColumnIdxs.empty());
}

bool CDataViewCtrl::AllowMultiColumnSort(bool allow)
{
	if (m_allowMultiColumnSort == allow)
		return true;

	m_allowMultiColumnSort = allow;

	// If disabling, must disable any multiple sort that are active
	if (!allow)
	{
		ResetAllSortColumns();

		if (CDataViewModel *model = GetModel())
			model->Resort();
	}

	return true;
}


bool CDataViewCtrl::IsColumnSorted(int idx) const
{
	for (wxVector<int>::const_iterator it = m_sortingColumnIdxs.begin(),
		end = m_sortingColumnIdxs.end();
		it != end;
		++it)
	{
		if (*it == idx)
			return true;
	}

	return false;
}

void CDataViewCtrl::UseColumnForSorting(int idx)
{
	m_sortingColumnIdxs.push_back(idx);
}

void CDataViewCtrl::DontUseColumnForSorting(int idx)
{
	for (wxVector<int>::iterator it = m_sortingColumnIdxs.begin(),
		end = m_sortingColumnIdxs.end();
		it != end;
		++it)
	{
		if (*it == idx)
		{
			m_sortingColumnIdxs.erase(it);
			return;
		}
	}

	wxFAIL_MSG("Column is not used for sorting");
}

void CDataViewCtrl::ToggleSortByColumn(int column)
{
	m_headerArea->ToggleSortByColumn(column);
}

void CDataViewCtrl::DoEnableSystemTheme(bool enable, wxWindow* window)
{
	typedef wxSystemThemedControl<wxControl> Base;
	Base::DoEnableSystemTheme(enable, window);
	Base::DoEnableSystemTheme(enable, m_clientArea);
	if (m_headerArea)
		Base::DoEnableSystemTheme(enable, m_headerArea);
}

#if wxUSE_ACCESSIBILITY
wxAccessible* CDataViewCtrl::CreateAccessible()
{
	return new CDataViewCtrlAccessible(this);
}
#endif // wxUSE_ACCESSIBILITY

#if wxUSE_ACCESSIBILITY
//-----------------------------------------------------------------------------
// CDataViewCtrlAccessible
//-----------------------------------------------------------------------------

CDataViewCtrlAccessible::CDataViewCtrlAccessible(CDataViewCtrl* win)
	: wxWindowAccessible(win)
{
}

// Can return either a child object, or an integer
// representing the child element, starting from 1.
wxAccStatus CDataViewCtrlAccessible::HitTest(const wxPoint& pt,
	int* childId, wxAccessible** childObject)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	wxDataViewItem item;
	CDataViewColumn* col;
	const wxPoint posCtrl = dvCtrl->ScreenToClient(pt);
	dvCtrl->HitTest(posCtrl, item, col);
	if (item.IsOk())
	{
		*childId = dvCtrl->GetRowByItem(item) + 1;
		*childObject = NULL;
	}
	else
	{
		if (((wxWindow*)dvCtrl)->HitTest(posCtrl) == wxHT_WINDOW_INSIDE)
		{
			// First check if provided point belongs to the header
			// because header control handles accesibility requestes on its own.
			wxHeaderCtrl* dvHdr = dvCtrl->GenericGetHeader();
			if (dvHdr)
			{
				const wxPoint posHdr = dvHdr->ScreenToClient(pt);
				if (dvHdr->HitTest(posHdr) == wxHT_WINDOW_INSIDE)
				{
					*childId = wxACC_SELF;
					*childObject = dvHdr->GetOrCreateAccessible();
					return wxACC_OK;
				}
			}

			*childId = wxACC_SELF;
			*childObject = this;
		}
		else
		{
			*childId = wxACC_SELF;
			*childObject = NULL;
		}
	}

	return wxACC_OK;
}

// Returns the rectangle for this object (id = 0) or a child element (id > 0).
wxAccStatus CDataViewCtrlAccessible::GetLocation(wxRect& rect, int elementId)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	if (elementId == wxACC_SELF)
	{
		// Header accesibility requestes are handled separately
		// so header is excluded from effective client area
		// and hence only main window area is reported.
		rect = dvWnd->GetScreenRect();
	}
	else
	{
		wxDataViewItem item = dvWnd->GetItemByRow(elementId - 1);
		if (!item.IsOk())
		{
			return wxACC_NOT_IMPLEMENTED;
		}

		rect = dvWnd->GetItemRect(item, NULL);
		// Indentation and expander column should be included here and therefore
		// reported row width should by the same as the width of the client area.
		rect.width += rect.x;
		rect.x = 0;
		wxPoint posScreen = dvWnd->ClientToScreen(rect.GetPosition());
		rect.SetPosition(posScreen);
	}

	return wxACC_OK;
}

// Navigates from fromId to toId/toObject.
wxAccStatus CDataViewCtrlAccessible::Navigate(wxNavDir navDir, int fromId,
	int* toId, wxAccessible** toObject)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	const int numRows = (int)dvWnd->GetRowCount();

	if (fromId == wxACC_SELF)
	{
		switch (navDir)
		{
		case wxNAVDIR_FIRSTCHILD:
			if (numRows > 0)
			{
				*toId = 1;
				*toObject = NULL;
				return wxACC_OK;
			}
			return wxACC_FALSE;
		case wxNAVDIR_LASTCHILD:
			if (numRows > 0)
			{
				*toId = numRows;
				*toObject = NULL;
				return wxACC_OK;
			}
			return wxACC_FALSE;
		case wxNAVDIR_DOWN:
			wxFALLTHROUGH;
		case wxNAVDIR_NEXT:
			wxFALLTHROUGH;
		case wxNAVDIR_UP:
			wxFALLTHROUGH;
		case wxNAVDIR_PREVIOUS:
			wxFALLTHROUGH;
		case wxNAVDIR_LEFT:
			wxFALLTHROUGH;
		case wxNAVDIR_RIGHT:
			// Standard wxWindow navigation is applicable here.
			return wxWindowAccessible::Navigate(navDir, fromId, toId, toObject);
		}
	}
	else
	{
		switch (navDir)
		{
		case wxNAVDIR_FIRSTCHILD:
			return wxACC_FALSE;
		case wxNAVDIR_LASTCHILD:
			return wxACC_FALSE;
		case wxNAVDIR_LEFT:
			return wxACC_FALSE;
		case wxNAVDIR_RIGHT:
			return wxACC_FALSE;
		case wxNAVDIR_DOWN:
			wxFALLTHROUGH;
		case wxNAVDIR_NEXT:
			if (fromId < numRows)
			{
				*toId = fromId + 1;
				*toObject = NULL;
				return wxACC_OK;
			}
			return wxACC_FALSE;
		case wxNAVDIR_PREVIOUS:
			wxFALLTHROUGH;
		case wxNAVDIR_UP:
			if (fromId > 1)
			{
				*toId = fromId - 1;
				*toObject = NULL;
				return wxACC_OK;
			}
			return wxACC_FALSE;
		}
	}

	// Let the framework handle the other cases.
	return wxACC_NOT_IMPLEMENTED;
}

// Gets the name of the specified object.
wxAccStatus CDataViewCtrlAccessible::GetName(int childId, wxString* name)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	if (childId == wxACC_SELF)
	{
		*name = dvCtrl->GetName();
	}
	else
	{
		wxDataViewItem item = dvCtrl->GetItemByRow(childId - 1);
		if (!item.IsOk())
		{
			return wxACC_NOT_IMPLEMENTED;
		}

		// Name is the value in the first textual column
		// plus the name of this column:
		// Column1: Value1
		wxString itemName;

		CDataViewModel* model = dvCtrl->GetModel();
		const unsigned int numCols = dvCtrl->GetColumnCount();
		for (unsigned int col = 0; col < numCols; col++)
		{
			CDataViewColumn *dvCol = dvCtrl->GetColumnAt(col);
			if (dvCol->IsHidden())
				continue; // skip it

			wxVariant value;
			model->GetValue(value, item, dvCol->GetModelColumn());
			if (value.IsNull() || value.IsType(wxS("bool")))
				continue; // Skip non-textual items

			wxDataViewRenderer* r = dvCol->GetRenderer();
			r->PrepareForItem(model, item, dvCol->GetModelColumn());
			wxString vs = r->GetAccessibleDescription();
			if (!vs.empty())
			{
				itemName = vs;
				break;
			}
		}

		if (itemName.empty())
		{
			// Return row number if no textual column found.
			// Rows are numbered from 1.
			*name = wxString::Format(_("Row %i"), childId);
		}
		else
		{
			*name = itemName;
		}
	}

	return wxACC_OK;
}

// Gets the number of children.
wxAccStatus CDataViewCtrlAccessible::GetChildCount(int* childCount)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	*childCount = (int)dvWnd->GetRowCount();
	return wxACC_OK;
}

// Gets the specified child (starting from 1).
// If *child is NULL and return value is wxACC_OK,
// this means that the child is a simple element and
// not an accessible object.
wxAccStatus CDataViewCtrlAccessible::GetChild(int childId, wxAccessible** child)
{
	*child = (childId == wxACC_SELF) ? this : NULL;
	return wxACC_OK;
}

// Performs the default action. childId is 0 (the action for this object)
// or > 0 (the action for a child).
// Return wxACC_NOT_SUPPORTED if there is no default action for this
// window (e.g. an edit control).
wxAccStatus CDataViewCtrlAccessible::DoDefaultAction(int childId)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	if (childId != wxACC_SELF)
	{
		CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);
		if (!dvWnd->IsList())
		{
			const unsigned int row = childId - 1;
			CDataViewTreeNode* node = dvWnd->GetTreeNodeByRow(row);
			if (node)
			{
				if (node->HasChildren())
				{
					// Expand or collapse the node.
					if (node->IsOpen())
						dvWnd->Collapse(row);
					else
						dvWnd->Expand(row);
					return wxACC_OK;
				}
			}
		}
	}

	return wxACC_NOT_SUPPORTED;
}

// Gets the default action for this object (0) or > 0 (the action for a child).
// Return wxACC_OK even if there is no action. actionName is the action, or the empty
// string if there is no action.
// The retrieved string describes the action that is performed on an object,
// not what the object does as a result. For example, a toolbar button that prints
// a document has a default action of "Press" rather than "Prints the current document."
wxAccStatus CDataViewCtrlAccessible::GetDefaultAction(int childId, wxString* actionName)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	wxString action;
	if (childId != wxACC_SELF)
	{
		CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);
		if (!dvWnd->IsList())
		{
			CDataViewTreeNode* node = dvWnd->GetTreeNodeByRow(childId - 1);
			if (node)
			{
				if (node->HasChildren())
				{
					if (node->IsOpen())
						/* TRANSLATORS: Action for manipulating a tree control */
						action = _("Collapse");
					else
						/* TRANSLATORS: Action for manipulating a tree control */
						action = _("Expand");
				}
			}
		}
	}

	*actionName = action;
	return wxACC_OK;
}

// Returns the description for this object or a child.
wxAccStatus CDataViewCtrlAccessible::GetDescription(int childId, wxString* description)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	if (childId == wxACC_SELF)
	{
		CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);
		*description = wxString::Format(_("%s (%d items)"),
			dvCtrl->GetName().c_str(), dvWnd->GetRowCount());
	}
	else
	{
		wxDataViewItem item = dvCtrl->GetItemByRow(childId - 1);
		if (!item.IsOk())
		{
			return wxACC_NOT_IMPLEMENTED;
		}

		// Description is concatenation of the contents of items in all columns:
		// Column1: Value1, Column2: Value2, ...
		// First textual item should be skipped because it is returned
		// as a Name property.
		wxString itemDesc;

		bool firstTextSkipped = false;
		CDataViewModel* model = dvCtrl->GetModel();
		const unsigned int numCols = dvCtrl->GetColumnCount();
		for (unsigned int col = 0; col < numCols; col++)
		{
			if (!model->HasValue(item, col))
				continue; // skip it

			CDataViewColumn *dvCol = dvCtrl->GetColumnAt(col);
			if (dvCol->IsHidden())
				continue; // skip it

			wxVariant value;
			model->GetValue(value, item, dvCol->GetModelColumn());

			wxDataViewRenderer* r = dvCol->GetRenderer();
			r->PrepareForItem(model, item, dvCol->GetModelColumn());
			wxString valStr = r->GetAccessibleDescription();
			// Skip first textual item
			if (!firstTextSkipped && !value.IsNull() && !value.IsType(wxS("bool")) && !valStr.empty())
			{
				firstTextSkipped = true;
				continue;
			}

			if (!valStr.empty())
			{
				wxString colName = dvCol->GetTitle();
				// If column has no label then present its index.
				if (colName.empty())
				{
					// Columns are numbered from 1.
					colName = wxString::Format(_("Column %u"), col + 1);
				}

				if (!itemDesc.empty())
					itemDesc.Append(wxS(", "));
				itemDesc.Append(colName);
				itemDesc.Append(wxS(": "));
				itemDesc.Append(valStr);
			}
		}

		*description = itemDesc;
	}

	return wxACC_OK;
}

// Returns help text for this object or a child, similar to tooltip text.
wxAccStatus CDataViewCtrlAccessible::GetHelpText(int childId, wxString* helpText)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

#if wxUSE_HELP
	if (childId == wxACC_SELF)
	{
		*helpText = dvCtrl->GetHelpText();
	}
	else
	{
		wxDataViewItem item = dvCtrl->GetItemByRow(childId - 1);
		if (item.IsOk())
		{
			CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);
			wxRect rect = dvWnd->GetItemRect(item, NULL);
			*helpText = dvWnd->GetHelpTextAtPoint(rect.GetPosition(), wxHelpEvent::Origin_Keyboard);
		}
		else
		{
			helpText->clear();
		}
	}
	return wxACC_OK;
#else
	(void)childId;
	(void)helpText;
	return wxACC_NOT_IMPLEMENTED;
#endif
}

// Returns the keyboard shortcut for this object or child.
// Return e.g. ALT+K
wxAccStatus CDataViewCtrlAccessible::GetKeyboardShortcut(int childId, wxString* shortcut)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	if (childId != wxACC_SELF)
	{
		CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);
		if (!dvWnd->IsList())
		{
			CDataViewTreeNode* node = dvWnd->GetTreeNodeByRow(childId - 1);
			if (node)
			{
				if (node->HasChildren())
				{
					if (node->IsOpen())
						/* TRANSLATORS: Keystroke for manipulating a tree control */
						*shortcut = _("Left");
					else
						/* TRANSLATORS: Keystroke for manipulating a tree control */
						*shortcut = _("Right");

					return wxACC_OK;
				}
			}
		}
	}

	return wxACC_FALSE;
}

// Returns a role constant.
wxAccStatus CDataViewCtrlAccessible::GetRole(int childId, wxAccRole* role)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	if (childId == wxACC_SELF)
		*role = dvWnd->IsList() ? wxROLE_SYSTEM_LIST : wxROLE_SYSTEM_OUTLINE;
	else
		*role = dvWnd->IsList() ? wxROLE_SYSTEM_LISTITEM : wxROLE_SYSTEM_OUTLINEITEM;

	return wxACC_OK;
}

// Returns a state constant.
wxAccStatus CDataViewCtrlAccessible::GetState(int childId, long* state)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	long st = 0;
	// State flags common to the object and its children.
	if (!dvWnd->IsEnabled())
		st |= wxACC_STATE_SYSTEM_UNAVAILABLE;
	if (!dvWnd->IsShown())
		st |= wxACC_STATE_SYSTEM_INVISIBLE;

	if (childId == wxACC_SELF)
	{
		if (dvWnd->IsFocusable())
			st |= wxACC_STATE_SYSTEM_FOCUSABLE;
		if (dvWnd->HasFocus())
			st |= wxACC_STATE_SYSTEM_FOCUSED;
	}
	else
	{
		const unsigned int rowNum = childId - 1;

		if (dvWnd->IsFocusable())
			st |= wxACC_STATE_SYSTEM_FOCUSABLE | wxACC_STATE_SYSTEM_SELECTABLE;
		if (!dvWnd->IsSingleSel())
			st |= wxACC_STATE_SYSTEM_MULTISELECTABLE | wxACC_STATE_SYSTEM_EXTSELECTABLE;

		if (rowNum < dvWnd->GetFirstVisibleRow() || rowNum > dvWnd->GetLastFullyVisibleRow())
			st |= wxACC_STATE_SYSTEM_OFFSCREEN;
		if (dvWnd->GetCurrentRow() == rowNum)
			st |= wxACC_STATE_SYSTEM_FOCUSED;
		if (dvWnd->IsRowSelected(rowNum))
			st |= wxACC_STATE_SYSTEM_SELECTED;

		if (!dvWnd->IsList())
		{
			CDataViewTreeNode* node = dvWnd->GetTreeNodeByRow(rowNum);
			if (node)
			{
				if (node->HasChildren())
				{
					if (node->IsOpen())
						st |= wxACC_STATE_SYSTEM_EXPANDED;
					else
						st |= wxACC_STATE_SYSTEM_COLLAPSED;
				}
			}
		}
	}
	*state = st;
	return wxACC_OK;
}

// Returns a localized string representing the value for the object
// or child.
wxAccStatus CDataViewCtrlAccessible::GetValue(int childId, wxString* strValue)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	wxString val;

	if (childId != wxACC_SELF)
	{
		CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);
		if (!dvWnd->IsList())
		{
			// In the tree view each item within the control has a zero-based value
			// that represents its level within the hierarchy and this value
			// is returned as a Value property.
			CDataViewTreeNode *node = dvWnd->GetTreeNodeByRow(childId - 1);
			if (node)
			{
				val = wxString::Format(wxS("%i"), node->GetIndentLevel());
			}
		}
	}

	*strValue = val;
	return wxACC_OK;
}

// Selects the object or child.
wxAccStatus CDataViewCtrlAccessible::Select(int childId, wxAccSelectionFlags selectFlags)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	if (childId == wxACC_SELF)
	{
		if (selectFlags == wxACC_SEL_TAKEFOCUS)
		{
			dvWnd->SetFocus();
		}
		else if (selectFlags != wxACC_SEL_NONE)
		{
			wxFAIL_MSG(wxS("Invalid selection flag"));
			return wxACC_INVALID_ARG;
		}
	}
	else
	{
		// These flags are not allowed in the single-selection mode:
		if (dvWnd->IsSingleSel() &&
			selectFlags & (wxACC_SEL_EXTENDSELECTION | wxACC_SEL_ADDSELECTION | wxACC_SEL_REMOVESELECTION))
		{
			wxFAIL_MSG(wxS("Invalid selection flag"));
			return wxACC_INVALID_ARG;
		}

		const int row = childId - 1;

		if (selectFlags == wxACC_SEL_TAKEFOCUS)
		{
			dvWnd->ChangeCurrentRow(row);
		}
		else if (selectFlags & wxACC_SEL_TAKESELECTION)
		{
			// This flag must not be combined with the following flags:
			if (selectFlags & (wxACC_SEL_EXTENDSELECTION | wxACC_SEL_ADDSELECTION | wxACC_SEL_REMOVESELECTION))
			{
				wxFAIL_MSG(wxS("Invalid selection flag"));
				return wxACC_INVALID_ARG;
			}

			dvWnd->UnselectAllRows();
			dvWnd->SelectRow(row, true);
			if (selectFlags & wxACC_SEL_TAKEFOCUS || dvWnd->IsSingleSel())
			{
				dvWnd->ChangeCurrentRow(row);
			}
		}
		else if (selectFlags & wxACC_SEL_EXTENDSELECTION)
		{
			// This flag must not be combined with the following flag:
			if (selectFlags & wxACC_SEL_TAKESELECTION)
			{
				wxFAIL_MSG(wxS("Invalid selection flag"));
				return wxACC_INVALID_ARG;
			}
			// These flags cannot be set together:
			if ((selectFlags & (wxACC_SEL_ADDSELECTION | wxACC_SEL_REMOVESELECTION))
				== (wxACC_SEL_ADDSELECTION | wxACC_SEL_REMOVESELECTION))
			{
				wxFAIL_MSG(wxS("Invalid selection flag"));
				return wxACC_INVALID_ARG;
			}

			// We have to have a focused object as a selection anchor.
			unsigned int focusedRow = dvWnd->GetCurrentRow();
			if (focusedRow == (unsigned int)-1)
			{
				wxFAIL_MSG(wxS("No selection anchor"));
				return wxACC_INVALID_ARG;
			}

			bool doSelect;
			if (selectFlags & wxACC_SEL_ADDSELECTION)
				doSelect = true;
			else if (selectFlags & wxACC_SEL_REMOVESELECTION)
				doSelect = false;
			else
				// If the anchor object is selected, the selection is extended.
				// If the anchor object is not selected, all objects are unselected.
				doSelect = dvWnd->IsRowSelected(focusedRow);

			if (doSelect)
			{
				dvWnd->SelectRows(focusedRow, row);
			}
			else
			{
				for (int r = focusedRow; r <= row; r++)
					dvWnd->SelectRow(r, false);
			}

			if (selectFlags & wxACC_SEL_TAKEFOCUS)
			{
				dvWnd->ChangeCurrentRow(row);
			}
		}
		else if (selectFlags & wxACC_SEL_ADDSELECTION)
		{
			// This flag must not be combined with the following flags:
			if (selectFlags & (wxACC_SEL_TAKESELECTION | wxACC_SEL_REMOVESELECTION))
			{
				wxFAIL_MSG(wxS("Invalid selection flag"));
				return wxACC_INVALID_ARG;
			}

			// Combination with wxACC_SEL_EXTENDSELECTION is already handled
			// (see wxACC_SEL_EXTENDSELECTION block).
			dvWnd->SelectRow(row, true);
			if (selectFlags & wxACC_SEL_TAKEFOCUS)
			{
				dvWnd->ChangeCurrentRow(row);
			}
		}
		else if (selectFlags & wxACC_SEL_REMOVESELECTION)
		{
			// This flag must not be combined with the following flags:
			if (selectFlags & (wxACC_SEL_TAKESELECTION | wxACC_SEL_ADDSELECTION))
			{
				wxFAIL_MSG(wxS("Invalid selection flag"));
				return wxACC_INVALID_ARG;
			}

			// Combination with wxACC_SEL_EXTENDSELECTION is already handled
			// (see wxACC_SEL_EXTENDSELECTION block).
			dvWnd->SelectRow(row, false);
			if (selectFlags & wxACC_SEL_TAKEFOCUS)
			{
				dvWnd->ChangeCurrentRow(row);
			}
		}
	}

	return wxACC_OK;
}

// Gets the window with the keyboard focus.
// If childId is 0 and child is NULL, no object in
// this subhierarchy has the focus.
// If this object has the focus, child should be 'this'.
wxAccStatus CDataViewCtrlAccessible::GetFocus(int* childId, wxAccessible** child)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);
	CDataViewMainWindow* dvWnd = wxDynamicCast(dvCtrl->GetMainWindow(), CDataViewMainWindow);

	const unsigned int row = dvWnd->GetCurrentRow();
	if (row != (unsigned int)*childId - 1)
	{
		*childId = row + 1;
		*child = NULL;
	}
	else
	{
		// First check if header is focused because header control
		// handles accesibility requestes on its own.
		wxHeaderCtrl* dvHdr = dvCtrl->GenericGetHeader();
		if (dvHdr)
		{
			if (dvHdr->HasFocus())
			{
				*childId = wxACC_SELF;
				*child = dvHdr->GetOrCreateAccessible();
				return wxACC_OK;
			}
		}

		if (dvWnd->HasFocus())
		{
			*childId = wxACC_SELF;
			*child = this;
		}
		else
		{
			*childId = 0;
			*child = NULL;
		}
	}

	return wxACC_OK;
}

// Gets a variant representing the selected children
// of this object.
// Acceptable values:
// - a null variant (IsNull() returns true)
// - a "void*" pointer to a wxAccessible child object
// - an integer representing the selected child element,
//   or 0 if this object is selected (GetType() == wxT("long"))
// - a list variant (GetType() == wxT("list"))
wxAccStatus CDataViewCtrlAccessible::GetSelections(wxVariant* selections)
{
	CDataViewCtrl* dvCtrl = wxDynamicCast(GetWindow(), CDataViewCtrl);
	wxCHECK(dvCtrl, wxACC_FAIL);

	wxDataViewItemArray sel;
	dvCtrl->GetSelections(sel);
	if (sel.IsEmpty())
	{
		selections->MakeNull();
	}
	else
	{
		wxVariantList tempList;
		wxVariant v(tempList);

		for (size_t i = 0; i < sel.GetCount(); i++)
		{
			int row = dvCtrl->GetRowByItem(sel[i]);
			v.Append(wxVariant((long)row + 1));
		}

		// Don't return the list if one child is selected.
		if (v.GetCount() == 1)
			*selections = wxVariant(v[0].GetLong());
		else
			*selections = v;
	}

	return wxACC_OK;
}
#endif // wxUSE_ACCESSIBILITY

#endif // !wxHAS_GENERIC_DATAVIEWCTRL

#endif // wxUSE_DATAVIEWCTRL
