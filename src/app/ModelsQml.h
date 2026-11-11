// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/FavoritesGroupsModel.h"
#include "models/FavoritesModel.h"
#include "models/FilterOptionsModel.h"
#include "models/GenreRailsModel.h"
#include "models/HomeRowsModel.h"
#include "models/ItemChildrenModel.h"
#include "models/ItemDetailsModel.h"
#include "models/ItemRowModel.h"
#include "models/LibraryItemsModel.h"
#include "models/LibrarySubtabController.h"
#include "models/PlayQueue.h"
#include "models/RecommendationRowsModel.h"
#include "models/ResumeItemsModel.h"
#include "models/RemoteSessionsModel.h"
#include "models/SearchModel.h"
#include "models/SeasonEpisodesModel.h"
#include "models/UserConfigurationModel.h"

#include <QtQml/qqmlregistration.h>

// Makes the core library's models known to QML without linking the core to QtQml
namespace Kinefin {
struct LibraryItemsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::LibraryItemsModel)
    QML_NAMED_ELEMENT(LibraryItemsModel)
    QML_UNCREATABLE("Created by the application")
};

struct LibrarySubtabControllerForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::LibrarySubtabController)
    QML_NAMED_ELEMENT(LibrarySubtabController)
    QML_UNCREATABLE("Created by the application")
};

struct RecommendationRowsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::RecommendationRowsModel)
    QML_NAMED_ELEMENT(RecommendationRowsModel)
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

struct FilterOptionsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::FilterOptionsModel)
    QML_NAMED_ELEMENT(FilterOptionsModel)
    QML_UNCREATABLE("Created by the application")
};

struct PlayQueueForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::PlayQueue)
    QML_NAMED_ELEMENT(PlayQueue)
    QML_UNCREATABLE("Created by the application")
};

struct GenreRailsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::GenreRailsModel)
    QML_NAMED_ELEMENT(GenreRailsModel)
    QML_UNCREATABLE("Created by the application")
};

struct RemoteSessionsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::RemoteSessionsModel)
    QML_NAMED_ELEMENT(RemoteSessionsModel)
    QML_UNCREATABLE("Created by the application")
};

struct ResumeItemsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::ResumeItemsModel)
    QML_NAMED_ELEMENT(ResumeItemsModel)
    QML_UNCREATABLE("Created by the application")
};

struct UserConfigurationModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::UserConfigurationModel)
    QML_NAMED_ELEMENT(UserConfigurationModel)
    QML_UNCREATABLE("Created by the application")
};

struct CultureOptionsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::CultureOptionsModel)
    QML_NAMED_ELEMENT(CultureOptionsModel)
    QML_UNCREATABLE("Created by the application")
};

struct FavoritesModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::FavoritesModel)
    QML_NAMED_ELEMENT(FavoritesModel)
    QML_UNCREATABLE("Created by the application")
};

struct FavoritesGroupsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::FavoritesGroupsModel)
    QML_NAMED_ELEMENT(FavoritesGroupsModel)
    QML_UNCREATABLE("Created by the application")
};
} // namespace Kinefin
