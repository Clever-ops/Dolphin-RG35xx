#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "Core/ConfigManager.h"
#include "SettingsPages.h"


SettingPage::SettingPage(QWidget *parent)
    : QWidget(parent)
{

}

void GeneralPage::LoadConfig()
{
    const SConfig& startup_params = SConfig::GetInstance();

    enableDualCore->setChecked(startup_params.bCPUThread);
    enableIdleSkip->setChecked(startup_params.bSkipIdle);
    enableCheats->setChecked(startup_params.bEnableCheats);
    forceNTSC->setChecked(startup_params.bForceNTSCJ);

   /* // Set Speed Limit selected item
    u32 selection = qRound(startup_params.m_EmulationSpeed * 10.0f);
    if (selection < speedLimit->size())
        speedLimit->SetSelection(selection);
    */

    switch(SConfig::GetInstance().iCPUCore)
    {
        case 0:
            cpu_Interpreter->click();
            break;
        case 5:
            cpu_CachedInterpreter->click();
            break;
        case 1:
            cpu_JITRecompiler->click();
            break;
        case 2:
            cpu_JITILRecompiler->click();
            break;
        default:
            cpu_JITRecompiler->click();
            break;
    }
}

void GeneralPage::SaveConfig()
{
    //const SConfig& save_params = SConfig::GetInstance();
}

GeneralPage::GeneralPage()
{
    QGroupBox *configGroup = new QGroupBox(tr("General configuration"));;
    QVBoxLayout *configLayout = new QVBoxLayout;

    {
        QGroupBox *basicGroup = new QGroupBox(tr("Basic Settings"));
        configLayout->addWidget(basicGroup);

        enableDualCore = new QCheckBox(tr("Enable Dual Core (speedup)"));
        enableIdleSkip = new QCheckBox(tr("Enable Idle Skipping (speedup)"));
        enableCheats   = new QCheckBox(tr("Enable Cheats"));

        // Set values.
        speedLimit = new QComboBox;

        speedLimit->addItem(tr("Unlimited"));
        for (int i = 10; i <= 200; i += 10) // from 10% to 200%
        {
            QString str;
            if (i == 100)
                str.sprintf("%i%% (Normal Speed)",i);
            else
                str.sprintf("%i%%",i);

            speedLimit->addItem(str);
        }

        QVBoxLayout *basicGroupLayout = new QVBoxLayout;

        basicGroupLayout->addWidget(enableDualCore);
        basicGroupLayout->addWidget(enableIdleSkip);
        basicGroupLayout->addWidget(enableCheats);
        basicGroupLayout->addWidget(speedLimit);
        basicGroupLayout->addStretch(1);

        basicGroup->setLayout(basicGroupLayout);
    }
    {
        QGroupBox *advancedGroup = new QGroupBox(tr("Advanced Settings"));
        configLayout->addWidget(advancedGroup);

        QVBoxLayout *advancedGroupLayout = new QVBoxLayout;

        {
            QGroupBox *cpuGroup = new QGroupBox(tr("CPU Emulator Engine"));
            advancedGroupLayout->addWidget(cpuGroup);

            QVBoxLayout *cpuGroupLayout = new QVBoxLayout;

            cpu_Interpreter = new QRadioButton(tr("Interpreter (slowest)"));
            cpu_CachedInterpreter = new QRadioButton(tr("Cached Interpreter (slower)"));
            cpu_JITRecompiler = new QRadioButton(tr("JIT Recompiler (recommended)"));
            cpu_JITILRecompiler = new QRadioButton(tr("JITIL Recompiler (slow, experimental)"));

            cpuGroupLayout->addWidget(cpu_Interpreter);
            cpuGroupLayout->addWidget(cpu_CachedInterpreter);
            cpuGroupLayout->addWidget(cpu_JITRecompiler);
            cpuGroupLayout->addWidget(cpu_JITILRecompiler);
            cpuGroupLayout->addStretch(1);

            cpuGroup->setLayout(cpuGroupLayout);

        }
        forceNTSC   = new QCheckBox(tr("Force Console as NTSC-J"));

        advancedGroupLayout->addWidget(forceNTSC);
        advancedGroupLayout->addStretch(1);

        advancedGroup->setLayout(advancedGroupLayout);
    }

    configGroup->setLayout(configLayout);
    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addWidget(configGroup);
    mainLayout->addStretch(1);

    LoadConfig();

    setLayout(mainLayout);
}