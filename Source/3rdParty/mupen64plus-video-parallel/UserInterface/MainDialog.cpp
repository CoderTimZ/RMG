/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "MainDialog.hpp"
#include "m64p_types.h"
#include "m64p_config.h"
#include "../gfx_m64p.h"

#include <algorithm>

using namespace UserInterface;

MainDialog::MainDialog(QWidget* parent) : QDialog(parent)
{
    this->setupUi(this);

    // window scale combobox: 1x to 4x
    int windowScale = ConfigGetParamInt(configVideoParallel, KEY_WINDOW_SCALE);
    this->windowScaleComboBox->setCurrentIndex(std::clamp(windowScale, 1, 4) - 1);

    // set upscaling combobox
    int upscaleAmount = ConfigGetParamInt(configVideoParallel, KEY_UPSCALING);
    int comboBoxIndex = 0;
    switch (upscaleAmount)
    {
    case 1:
    case 2:
        comboBoxIndex = upscaleAmount - 1;
        break;
    case 4:
        comboBoxIndex = 2;
        break;
    case 8:
        comboBoxIndex = 3;
        break;
    default:
        this->resolutionUpscaleComboBox->addItem(QString::number(upscaleAmount) + "x");
        comboBoxIndex = 4;
        break;
    }
    this->resolutionUpscaleComboBox->setCurrentIndex(comboBoxIndex);

    this->deinterlacerComboBox->setCurrentIndex(ConfigGetParamBool(configVideoParallel, KEY_DEINTERLACE) ? 1 : 0);
    this->downscalerComboBox->setCurrentIndex(ConfigGetParamInt(configVideoParallel, KEY_DOWNSCALE));
    this->verticalStretchSpinBox->setValue(ConfigGetParamInt(configVideoParallel, KEY_VERTICAL_STRETCH));
    
    this->overscanGroupBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_OVERSCANCROPENABLE));
    this->overscanLeftSpinBox->setValue(ConfigGetParamInt(configVideoParallel, KEY_OVERSCANCROPLEFT));
    this->overscanRightSpinBox->setValue(ConfigGetParamInt(configVideoParallel, KEY_OVERSCANCROPRIGHT));
    this->overscanTopSpinBox->setValue(ConfigGetParamInt(configVideoParallel, KEY_OVERSCANCROPTOP));
    this->overscanBottomSpinBox->setValue(ConfigGetParamInt(configVideoParallel, KEY_OVERSCANCROPBOTTOM));

    this->superSampledDitherCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_SSDITHER));
    this->viAaCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_AA));
    this->viDivotFilterCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_DIVOT));
    this->viDeDitheringCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_VIDITHER));
    this->nativeTextureLodCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_NATIVETEXTLOD));
    this->forceWidescreenCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_WIDESCREEN));

    this->supersampledRdramReadsCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_SSREADBACKS));
    this->viBilinearFilteringCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_VIBILERP));
    this->viGammaDitherCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_GAMMADITHER));
    this->nativeTextRectsCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_NATIVETEXTRECT));
    this->verticalSyncCheckBox->setChecked(ConfigGetParamBool(configVideoParallel, KEY_VSYNC));
}

MainDialog::~MainDialog()
{
}

void MainDialog::on_buttonBox_clicked(QAbstractButton* button)
{
    QPushButton *pushButton = (QPushButton *)button;
    QPushButton *okButton = this->buttonBox->button(QDialogButtonBox::Ok);
    QPushButton *defaultButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);

    if (pushButton == okButton)
    {
        // window scale
        int windowScaleValue = this->windowScaleComboBox->currentIndex() + 1;
        ConfigSetParameter(configVideoParallel, KEY_WINDOW_SCALE, M64TYPE_INT, &windowScaleValue);

        // upscaling
        int upscalingIndex = this->resolutionUpscaleComboBox->currentIndex();
        int upscalingValue = 0;
        if (upscalingIndex == 0)
        {
            upscalingValue = 1;
        }
        else
        {
            upscalingValue = this->resolutionUpscaleComboBox->currentText().split("x").at(0).toInt();
        }
        ConfigSetParameter(configVideoParallel, KEY_UPSCALING, M64TYPE_INT, &upscalingValue);
    
        // deinterlacer
        int deinterlacerValue = this->deinterlacerComboBox->currentIndex();
        ConfigSetParameter(configVideoParallel, KEY_DEINTERLACE, M64TYPE_BOOL, &deinterlacerValue);

        // downscaler
        int downscalingValue = this->downscalerComboBox->currentIndex();
        ConfigSetParameter(configVideoParallel, KEY_DOWNSCALE, M64TYPE_INT, &downscalingValue);

        // cropOverscan
        int overscanEnabled = this->overscanGroupBox->isChecked();
        int overscanLeft = this->overscanLeftSpinBox->value();
        int overscanRight = this->overscanRightSpinBox->value();
        int overscanTop = this->overscanTopSpinBox->value();
        int overscanBottom = this->overscanBottomSpinBox->value();
        ConfigSetParameter(configVideoParallel, KEY_OVERSCANCROPENABLE, M64TYPE_BOOL, &overscanEnabled);
        ConfigSetParameter(configVideoParallel, KEY_OVERSCANCROPLEFT, M64TYPE_INT, &overscanLeft);
        ConfigSetParameter(configVideoParallel, KEY_OVERSCANCROPRIGHT, M64TYPE_INT, &overscanRight);
        ConfigSetParameter(configVideoParallel, KEY_OVERSCANCROPTOP, M64TYPE_INT, &overscanTop);
        ConfigSetParameter(configVideoParallel, KEY_OVERSCANCROPBOTTOM, M64TYPE_INT, &overscanBottom);

        // verticalStretch
        int verticalStretchValue = this->verticalStretchSpinBox->value();
        ConfigSetParameter(configVideoParallel, KEY_VERTICAL_STRETCH, M64TYPE_INT, &verticalStretchValue);

        // checkboxes
        int superSampledDitherValue = this->superSampledDitherCheckBox->isChecked() ? 1 : 0;
        int viAaValue = this->viAaCheckBox->isChecked() ? 1 : 0;
        int viDivotFilterValue = this->viDivotFilterCheckBox->isChecked() ? 1 : 0; 
        int viDeDitheringValue = this->viDeDitheringCheckBox->isChecked() ? 1 : 0;
        int nativeTextureLodValue = this->nativeTextureLodCheckBox->isChecked() ? 1 : 0; 
        int forceWidescreenValue = this->forceWidescreenCheckBox->isChecked() ? 1 : 0; 
        int supersampledRdramReadsValue = this->supersampledRdramReadsCheckBox->isChecked() ? 1 : 0;
        int viBilinearFilteringValue = this->viBilinearFilteringCheckBox->isChecked() ? 1 : 0;
        int viGammaDitherValue = this->viGammaDitherCheckBox->isChecked() ? 1 : 0;
        int nativeTextRectsValue = this->nativeTextRectsCheckBox->isChecked() ? 1 : 0;
        int verticalSyncValue = this->verticalSyncCheckBox->isChecked() ? 1 : 0;

        ConfigSetParameter(configVideoParallel, KEY_SSDITHER, M64TYPE_BOOL, &superSampledDitherValue);
        ConfigSetParameter(configVideoParallel, KEY_AA, M64TYPE_BOOL, &viAaValue);
        ConfigSetParameter(configVideoParallel, KEY_DIVOT, M64TYPE_BOOL, &viDivotFilterValue);
        ConfigSetParameter(configVideoParallel, KEY_VIDITHER, M64TYPE_BOOL, &viDeDitheringValue);
        ConfigSetParameter(configVideoParallel, KEY_NATIVETEXTLOD, M64TYPE_BOOL, &nativeTextureLodValue);
        ConfigSetParameter(configVideoParallel, KEY_WIDESCREEN, M64TYPE_BOOL, &forceWidescreenValue);
        ConfigSetParameter(configVideoParallel, KEY_SSREADBACKS, M64TYPE_BOOL, &supersampledRdramReadsValue);
        ConfigSetParameter(configVideoParallel, KEY_VIBILERP, M64TYPE_BOOL, &viBilinearFilteringValue);
        ConfigSetParameter(configVideoParallel, KEY_GAMMADITHER, M64TYPE_BOOL, &viGammaDitherValue);
        ConfigSetParameter(configVideoParallel, KEY_NATIVETEXTRECT, M64TYPE_BOOL, &nativeTextRectsValue);
        ConfigSetParameter(configVideoParallel, KEY_VSYNC, M64TYPE_BOOL, &verticalSyncValue);

        ConfigSaveSection("Video-Parallel");
    }
    else if (pushButton == defaultButton)
    {
        this->windowScaleComboBox->setCurrentIndex(0);
        this->resolutionUpscaleComboBox->setCurrentText("None");
        this->deinterlacerComboBox->setCurrentIndex(0);
        this->downscalerComboBox->setCurrentIndex(0);
        this->verticalStretchSpinBox->setValue(0);
        this->overscanGroupBox->setChecked(false);
        this->overscanLeftSpinBox->setValue(0);
        this->overscanRightSpinBox->setValue(0);
        this->overscanTopSpinBox->setValue(0);
        this->overscanBottomSpinBox->setValue(0);

        this->superSampledDitherCheckBox->setChecked(true);
        this->viAaCheckBox->setChecked(true);
        this->viDivotFilterCheckBox->setChecked(true);
        this->viDeDitheringCheckBox->setChecked(true);
        this->nativeTextureLodCheckBox->setChecked(false);
        this->forceWidescreenCheckBox->setChecked(false);
        this->supersampledRdramReadsCheckBox->setChecked(false);
        this->viBilinearFilteringCheckBox->setChecked(true);
        this->viGammaDitherCheckBox->setChecked(true);
        this->nativeTextRectsCheckBox->setChecked(true);
        this->verticalSyncCheckBox->setChecked(false);
    }
}
