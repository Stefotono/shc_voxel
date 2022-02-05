#ifndef VOXEL_RANGE_ANALYSIS_DIALOG_H
#define VOXEL_RANGE_ANALYSIS_DIALOG_H

#include <scene/gui/dialogs.h>

class CheckBox;
class SpinBox;
class GridContainer;

namespace zylann::voxel {

class VoxelRangeAnalysisDialog : public AcceptDialog {
	GDCLASS(VoxelRangeAnalysisDialog, AcceptDialog)
public:
	VoxelRangeAnalysisDialog();

	AABB get_aabb() const;
	bool is_analysis_enabled() const;

private:
	void _on_enabled_checkbox_toggled(bool enabled);
	void _on_area_spinbox_value_changed(float value);

	void add_row(String text, SpinBox *&sb, GridContainer *parent, float defval);

	static void _bind_methods();

	CheckBox *enabled_checkbox;
	SpinBox *pos_x_spinbox;
	SpinBox *pos_y_spinbox;
	SpinBox *pos_z_spinbox;
	SpinBox *size_x_spinbox;
	SpinBox *size_y_spinbox;
	SpinBox *size_z_spinbox;
};

} // namespace zylann::voxel

#endif // VOXEL_RANGE_ANALYSIS_DIALOG_H
