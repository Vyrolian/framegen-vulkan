/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <QStringList>

namespace mako::ui {

    /// get the list of available GPUs, automatically
    /// switching to PCI IDs if there are duplicates
    /// @return list of available GPUs
    QStringList getAvailableGPUs();

    /// Select the optional widget-theme bridge for qt6ct, preserving explicit
    /// Quick style/configuration choices and other platform themes.
    QString desktopQuickStyle(const QStringList& importPaths);

}
