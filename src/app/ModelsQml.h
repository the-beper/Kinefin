// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DownloadsController.h"
#include "RemoteCommands.h"
#include "models/CollectionsModel.h"
#include "models/DownloadsModel.h"
#include "models/FavoritesModel.h"
#include "models/FilterOptionsModel.h"
#include "models/GenreRailsModel.h"
#include "models/HomeRowsModel.h"
#include "models/ItemChildrenModel.h"
#include "models/ItemDetailsModel.h"
#include "models/ItemRowModel.h"
#include "models/LibraryItemsModel.h"
#include "models/LibrarySubtabController.h"
#include "models/PersonItemsModel.h"
#include "models/PlayQueue.h"
#include "models/PlaylistEntriesModel.h"
#include "models/PlaylistsController.h"
#include "models/RecommendationRowsModel.h"
#include "models/RemoteSessionsModel.h"
#include "models/ResumeItemsModel.h"
#include "models/SearchModel.h"
#include "models/SeasonEpisodesModel.h"
#include "models/UserConfigurationModel.h"
#include "syncplay/SyncPlayController.h"
#include "syncplay/SyncPlayGroupsModel.h"
#include "syncplay/SyncPlayQueueModel.h"

#include <QtQml/qqmlregistration.h>

// Makes the core library's models known to QML without linking the core to QtQml
namespace Kinefin {
struct DownloadsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::DownloadsModel)
    QML_NAMED_ELEMENT(DownloadsModel)
    QML_UNCREATABLE("Created by the application")
};

struct DownloadsControllerForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::DownloadsController)
    QML_NAMED_ELEMENT(DownloadsController)
    QML_UNCREATABLE("Created by the application")
};

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

struct SyncPlayControllerForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::SyncPlayController)
    QML_NAMED_ELEMENT(SyncPlayController)
    QML_UNCREATABLE("Created by the application")
};

struct SyncPlayQueueModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::SyncPlayQueueModel)
    QML_NAMED_ELEMENT(SyncPlayQueueModel)
    QML_UNCREATABLE("Created by the application")
};

struct SyncPlayGroupsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::SyncPlayGroupsModel)
    QML_NAMED_ELEMENT(SyncPlayGroupsModel)
    QML_UNCREATABLE("Created by the application")
};

struct ResumeItemsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::ResumeItemsModel)
    QML_NAMED_ELEMENT(ResumeItemsModel)
    QML_UNCREATABLE("Created by the application")
};

struct PersonItemsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::PersonItemsModel)
    QML_NAMED_ELEMENT(PersonItemsModel)
    QML_UNCREATABLE("Created by the application")
};

struct CollectionsModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::CollectionsModel)
    QML_NAMED_ELEMENT(CollectionsModel)
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

struct PlaylistsControllerForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::PlaylistsController)
    QML_NAMED_ELEMENT(PlaylistsController)
    QML_UNCREATABLE("Created by the application")
};

struct PlaylistEntriesModelForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::PlaylistEntriesModel)
    QML_NAMED_ELEMENT(PlaylistEntriesModel)
    QML_UNCREATABLE("Created by the application")
};

struct RemoteCommandsForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::RemoteCommands)
    QML_NAMED_ELEMENT(RemoteCommands)
    QML_UNCREATABLE("Created by the application")
};
} // namespace Kinefin
