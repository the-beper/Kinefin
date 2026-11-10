// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/HomeRowsModel.h"
#include "models/ItemChildrenModel.h"
#include "models/ItemDetailsModel.h"
#include "models/ItemRowModel.h"
#include "models/LibraryItemsModel.h"
#include "models/SearchModel.h"
#include "models/SeasonEpisodesModel.h"

#include <QtQml/qqmlregistration.h>

// Makes the core library's models known to QML without linking the core to QtQml
namespace Kinefin {
struct LibraryItemsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::LibraryItemsModel)
    QML_NAMED_ELEMENT(LibraryItemsModel)
    QML_UNCREATABLE("Created by the application")
};

struct HomeRowsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::HomeRowsModel)
    QML_NAMED_ELEMENT(HomeRowsModel)
    QML_UNCREATABLE("Created by the application")
};

struct ItemRowModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::ItemRowModel)
    QML_NAMED_ELEMENT(ItemRowModel)
    QML_UNCREATABLE("Created by the application")
};

struct ItemDetailsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::ItemDetailsModel)
    QML_NAMED_ELEMENT(ItemDetailsModel)
    QML_UNCREATABLE("Created by the application")
};

struct ItemChildrenModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::ItemChildrenModel)
    QML_NAMED_ELEMENT(ItemChildrenModel)
    QML_UNCREATABLE("Created by the application")
};

struct SeasonEpisodesModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::SeasonEpisodesModel)
    QML_NAMED_ELEMENT(SeasonEpisodesModel)
    QML_UNCREATABLE("Created by the application")
};

struct SearchModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::SearchModel)
    QML_NAMED_ELEMENT(SearchModel)
    QML_UNCREATABLE("Created by the application")
};
} // namespace Kinefin
