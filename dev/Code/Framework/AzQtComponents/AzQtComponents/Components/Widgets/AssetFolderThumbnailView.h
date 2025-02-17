/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#pragma once

#include <AzQtComponents/AzQtComponentsAPI.h>
#include <AzQtComponents/Components/Widgets/ScrollBar.h>
#include <QAbstractItemView>

class QSettings;

namespace AzQtComponents
{
    class Style;
    class AssetFolderThumbnailViewDelegate;

    class AZ_QT_COMPONENTS_API AssetFolderThumbnailView
        : public QAbstractItemView
    {
        Q_OBJECT
        Q_PROPERTY(ThumbnailSize thumbnailSize READ thumbnailSize WRITE setThumbnailSize)

    public:
        struct Config
        {
            struct Thumbnail
            {
                int smallSize;
                int mediumSize;
                int largeSize;
                qreal borderRadius;
                int padding;
                QColor backgroundColor;
                qreal borderThickness;
                qreal selectedBorderThickness;
                QColor borderColor;
                QColor selectedBorderColor;
            };

            struct ExpandButton
            {
                int width;
                qreal borderRadius;
                qreal caretWidth;
                QColor backgroundColor;
                QColor caretColor;
            };

            struct ChildFrame
            {
                int padding;
                qreal borderRadius;
                QColor backgroundColor;
            };

            int margin;
            Thumbnail rootThumbnail;
            Thumbnail childThumbnail;
            ExpandButton expandButton;
            ChildFrame childFrame;
        };

        static Config loadConfig(QSettings& settings);

        static Config defaultConfig();

        explicit AssetFolderThumbnailView(QWidget* parent = nullptr);
        virtual ~AssetFolderThumbnailView() override;

        enum class ThumbnailSize
        {
            Small,
            Medium,
            Large
        };
        Q_ENUM(ThumbnailSize)

        void setThumbnailSize(ThumbnailSize size);
        ThumbnailSize thumbnailSize() const;

        void updateGeometries() override;
        QModelIndex indexAt(const QPoint& point) const override;
        void scrollTo(const QModelIndex& index, QAbstractItemView::ScrollHint hint) override;
        QRect visualRect(const QModelIndex& index) const override;

    protected:
        friend class Style;

        static bool polish(Style* style, QWidget* widget, const ScrollBar::Config& scrollBarConfig, const Config& config);
        void polish(const Config& config);

        QModelIndex moveCursor(QAbstractItemView::CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
        int horizontalOffset() const override;
        int verticalOffset() const override;
        bool isIndexHidden(const QModelIndex& index) const override;
        void setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags flags) override;
        QRegion visualRegionForSelection(const QItemSelection& selection) const override;

        void paintEvent(QPaintEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;

    private:
        void paintChildFrames(QPainter* painter) const;
        void paintItems(QPainter* painter) const;

        bool isExpandable(const QModelIndex& index) const;

        int rootThumbnailSizeInPixels() const;
        int childThumbnailSizeInPixels() const;

        AssetFolderThumbnailViewDelegate* m_delegate;   
        AZ_PUSH_DISABLE_WARNING(4251, "-Wunknown-warning-option") // 'AzQtComponents::AssetFolderThumbnailView::m_itemGeometry': class 'QHash<QPersistentModelIndex,QRect>' needs to have dll-interface to be used by clients of class 'AzQtComponents::AssetFolderThumbnailView'
        QHash<QPersistentModelIndex, QRect> m_itemGeometry;
        struct ChildFrame
        {
            QPersistentModelIndex index;
            QVector<QRect> rects;
        };
        QVector<ChildFrame> m_childFrames;
        QSet<int> m_expandedRows;
        AZ_POP_DISABLE_WARNING
        ThumbnailSize m_thumbnailSize;
        Config m_config;
    };
}
