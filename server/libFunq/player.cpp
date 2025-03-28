/*
Copyright: SCLE SFE
Contributor: Julien Pagès <j.parkouss@gmail.com>

This software is a computer program whose purpose is to test graphical
applications written with the QT framework (http://qt.digia.com/).

This software is governed by the CeCILL v2.1 license under French law and
abiding by the rules of distribution of free software.  You can  use,
modify and/ or redistribute the software under the terms of the CeCILL
license as circulated by CEA, CNRS and INRIA at the following URL
"http://www.cecill.info".

As a counterpart to the access to the source code and  rights to copy,
modify and redistribute granted by the license, users are provided only
with a limited warranty  and the software's author,  the holder of the
economic rights,  and the successive licensors  have only  limited
liability.

In this respect, the user's attention is drawn to the risks associated
with loading,  using,  modifying and/or developing or reproducing the
software by the user in light of its specific status of free software,
that may mean  that it is complicated to manipulate,  and  that  also
therefore means  that it is reserved for developers  and  experienced
professionals having in-depth computer knowledge. Users are therefore
encouraged to load and test the software's suitability as regards their
requirements in conditions enabling the security of their systems and/or
data to be ensured and,  more generally, to use and operate it in the
same conditions as regards security.

The fact that you are presently reading this means that you have had
knowledge of the CeCILL v2.1 license and that you accept its terms.
*/

#include "player.h"

#include "dragndropresponse.h"
#include "objectpath.h"
#include "shortcutresponse.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QHeaderView>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QStringList>
#include <QTableView>
#include <QTime>
#include <QTimer>
#include <QTreeView>
#include <QWidget>
#include <QWindow>

#if QT_VERSION_MAJOR >= 6
#include <QScreen>
#else
#include <QDesktopWidget>
#endif

#ifdef QT_QUICK_LIB
#include <QQuickItem>
#include <QQuickWindow>
#endif

using namespace ObjectPath;

template <class T>
void mouse_click(T * w, const QPoint & pos, Qt::MouseButton button) {
    QPoint global_pos = w->mapToGlobal(pos);
    qApp->postEvent(w,
                    new QMouseEvent(QEvent::MouseButtonPress, pos, global_pos,
                                    button, Qt::NoButton, Qt::NoModifier));
    qApp->postEvent(w,
                    new QMouseEvent(QEvent::MouseButtonRelease, pos, global_pos,
                                    button, Qt::NoButton, Qt::NoModifier));
}

template <class T>
void mouse_dclick(T * w, const QPoint & pos) {
    mouse_click(w, pos, Qt::LeftButton);
    qApp->postEvent(
        w,
        new QMouseEvent(QEvent::MouseButtonDblClick, pos, w->mapToGlobal(pos),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier));
}

void activate_focus(QWidget * w) {
    w->activateWindow();
    w->setFocus(Qt::MouseFocusReason);
}

void dump_properties(QObject * object, QtJson::JsonObject & out) {
    const QMetaObject * metaobject = object->metaObject();
    for (int i = 0; i < metaobject->propertyCount(); ++i) {
        QMetaProperty prop = metaobject->property(i);
        QVariant value = object->property(prop.name());
        // first try to serialize and only add property if it is possible
        bool success = false;
        QtJson::serialize(value, success);
        if (success) {
            out[prop.name()] = value;
        }
    }
}

void dump_object(QObject * object, QtJson::JsonObject & out,
                 bool with_properties = false) {
    out["path"] = objectPath(object);
    QStringList classes;
    const QMetaObject * mo = object->metaObject();
    while (mo) {
        // sometimes classes appears twice
        if (!classes.contains(mo->className())) {
            classes << mo->className();
        }
        mo = mo->superClass();
    }
    out["classes"] = classes;
    if (with_properties) {
        QtJson::JsonObject properties;
        dump_properties(object, properties);
        out["properties"] = properties;
    }
}

QString item_model_path(QAbstractItemModel * model, const QModelIndex & item) {
    QStringList path;
    QModelIndex parent = model->parent(item);
    while (parent.isValid()) {
        path << (QString::number(parent.row()) + "-" +
                 QString::number(parent.column()));
        parent = model->parent(parent);
    }
    // reverse list
    for (int k = 0, s = path.size(), max = (s / 2); k < max; k++) {
#if QT_VERSION_MAJOR >= 6
        path.swapItemsAt(k, s - (1 + k));
#else
        path.swap(k, s - (1 + k));
#endif
    }
    return path.join("/");
}

void dump_item_model_attrs(QAbstractItemModel * model, QtJson::JsonObject & out,
                           const QModelIndex & index,
                           const qulonglong & modelId) {
    out["modelid"] = modelId;
    QString path = item_model_path(model, index);
    if (!path.isEmpty()) {
        out["itempath"] = path;
    }
    out["row"] = index.row();
    out["column"] = index.column();
    out["value"] = model->data(index).toString();

    QVariant checkable = model->data(index, Qt::CheckStateRole);
    if (checkable.isValid()) {
        Qt::CheckState state = static_cast<Qt::CheckState>(checkable.toUInt());
        QString stringState;
        switch (state) {
            case Qt::Unchecked:
                stringState = "unchecked";
                break;
            case Qt::PartiallyChecked:
                stringState = "partiallyChecked";
                break;
            case Qt::Checked:
                stringState = "checked";
                break;
        }
        out["check_state"] = stringState;
    }
}

void dump_items_model(QAbstractItemModel * model, QtJson::JsonObject & out,
                      const QModelIndex & parent, const qulonglong & modelId,
                      bool recursive = true) {
    QtJson::JsonArray items;
    for (int i = 0; i < model->rowCount(parent); ++i) {
        for (int j = 0; j < model->columnCount(parent); ++j) {
            QModelIndex index = model->index(i, j, parent);
            QtJson::JsonObject item;
            dump_item_model_attrs(model, item, index, modelId);
            if (j == 0 && recursive && model->hasChildren(index)) {
                dump_items_model(model, item, index, modelId);
            }
            items << item;
        }
    }
    out["items"] = items;
}

QModelIndex get_model_item(QAbstractItemModel * model, const QString & path,
                           int row, int column) {
    QModelIndex parent;
    if (!path.isEmpty()) {
        QStringList parts = path.split("/");
        foreach (const QString & part, parts) {
            QStringList tmp = part.split("-");
            if (tmp.count() != 2) {
                return QModelIndex();
            }
            parent = model->index(tmp.at(0).toInt(), tmp.at(1).toInt(), parent);
            if (!parent.isValid()) {
                return parent;
            }
        }
    }

    return model->index(row, column, parent);
}

void dump_graphics_items(const QList<QGraphicsItem *> & items,
                         const qulonglong & viewid, QtJson::JsonObject & out) {
    QtJson::JsonArray outitems;
    foreach (QGraphicsItem * item, items) {
        QtJson::JsonObject outitem;
        outitem["gid"] = graphicsItemId(item);
        outitem["viewid"] = viewid;
        QObject * itemObject = dynamic_cast<QObject *>(item);
        if (itemObject) {
            const QMetaObject * mo = itemObject->metaObject();
            QStringList classes;
            while (mo) {
                classes << mo->className();
                mo = mo->superClass();
            }
            outitem["classes"] = classes;
            outitem["objectname"] = itemObject->objectName();
        }
        dump_graphics_items(item->childItems(), viewid, outitem);
        outitems << outitem;
    }
    out["items"] = outitems;
}

Player::Player(QIODevice * device, QObject * parent)
    : JsonClient(device, parent) {
}

qulonglong Player::registerObject(QObject * object) {
    if (!object) {
        return 0;
    }
    qulonglong id = (qulonglong)object;
    if (!m_registeredObjects.contains(id)) {
        connect(object, SIGNAL(destroyed(QObject *)), this,
                SLOT(objectDeleted(QObject *)));
        m_registeredObjects[id] = object;
    }
    return id;
}

QObject * Player::registeredObject(const qulonglong & id) {
    return m_registeredObjects[id];
}

void Player::objectDeleted(QObject * object) {
    qulonglong id = (qulonglong)object;
    m_registeredObjects.remove(id);
}

QtJson::JsonObject Player::list_commands(const QtJson::JsonObject &) {
    const QMetaObject * metaObject = this->metaObject();
    QStringList methods;
    for (int i = metaObject->methodOffset(); i < metaObject->methodCount();
         ++i) {
        QMetaMethod method = metaObject->method(i);
        if (method.methodType() == QMetaMethod::Slot) {
            methods << QString::fromLatin1(
                metaObject->method(i).methodSignature());
        }
    }
    QtJson::JsonObject result;
    result["commands"] = methods;
    return result;
}

QtJson::JsonObject Player::widget_by_path(const QtJson::JsonObject & command) {
    QString path = command["path"].toString();
    QObject * o = findObject(path);
    qulonglong id = registerObject(o);
    if (id == 0) {
        return createError(
            "InvalidWidgetPath",
            QString("Unable to find widget with path `%1`").arg(path));
    }
    QtJson::JsonObject result;
    result["oid"] = id;
    dump_object(o, result);
    return result;
}

QtJson::JsonObject Player::quick_item_find(const QtJson::JsonObject & command) {
    QtJson::JsonObject result;
#ifdef QT_QUICK_LIB
    WidgetLocatorContext<QQuickWindow> ctx(this, command, "quick_window_oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QQuickItem * item;
    qulonglong id;
    QString qid = command["qid"].toString();
    if (!qid.isEmpty()) {
        item = ObjectPath::findQuickItemById(ctx.widget->contentItem(), qid);
        id = registerObject(item);
        if (id == 0) {
            return createError(
                "InvalidQuickItem",
                QString("Unable to find quick item with id `%1`").arg(qid));
        }
    } else {
        QString path = command["path"].toString();
        item = ObjectPath::findQuickItem(ctx.widget, path);
        id = registerObject(item);
        if (id == 0) {
            return createError(
                "InvalidQuickItem",
                QString("Unable to find quick item with path `%1`").arg(path));
        }
    }
    result["oid"] = id;
    result["quick_window_oid"] = command["quick_window_oid"].toString();
    dump_object(item, result);
#else
    Q_UNUSED(command);
    result = createQtQuickOnlyError();
#endif
    return result;
}

QtJson::JsonObject Player::active_widget(const QtJson::JsonObject & command) {
    QObject * active;
    QString type = command["type"].toString();
    if (type == "modal") {
        active = QApplication::activeModalWidget();
        if (!active) {
            active = QApplication::modalWindow();
        }
    } else if (type == "popup") {
        active = QApplication::activePopupWidget();
    } else if (type == "focus") {
        active = QApplication::focusWidget();
        if (!active) {
            active = QApplication::focusWindow();
        }
    } else {
        active = QApplication::activeWindow();
        if (!active) {
            QWindowList lst = QGuiApplication::topLevelWindows();
            if (!lst.isEmpty()) {
                active = lst.first();
            }
        }
    }
    if (!active) {
        return createError(
            "NoActiveWindow",
            QString::fromUtf8("There is no active widget (%1)").arg(type));
    }
    qulonglong id = registerObject(active);
    QtJson::JsonObject result;
    result["oid"] = id;
    dump_object(active, result);
    return result;
}

ObjectLocatorContext::ObjectLocatorContext(Player * player,
                                           const QtJson::JsonObject & command,
                                           const QString & oidKey) {
    id = command[oidKey].value<qulonglong>();
    obj = player->registeredObject(id);
    if (!obj) {
        lastError = player->createError(
            "NotRegisteredObject",
            QString::fromUtf8(
                "The object (id:%1) is not registered or has been destroyed")
                .arg(id));
    }
}

#ifdef QT_QUICK_LIB
QuickItemLocatorContext::QuickItemLocatorContext(
    Player * player, const QtJson::JsonObject & command, const QString & objKey)
    : ObjectLocatorContext(player, command, objKey) {
    if (!hasError()) {
        item = qobject_cast<QQuickItem *>(obj);
        if (!item) {
            lastError = player->createError(
                "NotAWidget",
                QString::fromUtf8("Object (id:%1) is not a QQuickItem")
                    .arg(id));
        } else {
            window = item->window();
            if (!window) {
                lastError = player->createError(
                    "NoWindowForQuickItem",
                    "No QQuickWindow associated to the item.");
            }
        }
    }
}
#endif

QtJson::JsonObject Player::object_properties(
    const QtJson::JsonObject & command) {
    ObjectLocatorContext ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QtJson::JsonObject result;
    dump_properties(ctx.obj, result);
    return result;
}

QtJson::JsonObject Player::object_set_properties(
    const QtJson::JsonObject & command) {
    ObjectLocatorContext ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QVariantMap properties = command["properties"].value<QVariantMap>();
    _object_set_properties(ctx.obj, properties);
    QtJson::JsonObject result;
    return result;
}

void Player::_object_set_properties(QObject * object,
                                    const QVariantMap & properties) {
    for (QtJson::JsonObject::const_iterator iter = properties.begin();
         iter != properties.end(); ++iter) {
        object->setProperty(iter.key().toStdString().c_str(), iter.value());
    }
}

void recursive_list_widget(QWidget * widget, QtJson::JsonObject & out,
                           bool with_properties, bool recursive) {
    QtJson::JsonObject resultWidgets, resultWidget;
    dump_object(widget, resultWidget, with_properties);
    foreach (QObject * obj, widget->children()) {
        QWidget * subWidget = qobject_cast<QWidget *>(obj);
        if (recursive && subWidget) {
            recursive_list_widget(subWidget, resultWidgets, with_properties, recursive);
        }
    }
    resultWidget["children"] = resultWidgets;
    out[objectName(widget)] = resultWidget;
}

QtJson::JsonObject Player::widgets_list(const QtJson::JsonObject & command) {
    bool with_properties = command["with_properties"].toBool();
    bool recursive = command["recursive"].toBool();
    QtJson::JsonObject result;
    if (command.contains("oid")) {
        ObjectLocatorContext ctx(this, command, "oid");
        if (ctx.hasError()) {
            return ctx.lastError;
        }
        foreach (QObject * obj, ctx.obj->children()) {
            QWidget * subWidget = qobject_cast<QWidget *>(obj);
            if (subWidget) {
                recursive_list_widget(subWidget, result, with_properties, recursive);
            }
        }
    } else {
        QList<QWidget *> widgets = QApplication::topLevelWidgets();
        if (!widgets.isEmpty()) {
            foreach (QWidget * widget, widgets) {
                recursive_list_widget(widget, result, with_properties, recursive);
            }
        } else {
            // no qwidgets, this is probably a qtquick app - anyway, check for
            // windows
            foreach (QWindow * window, QApplication::topLevelWindows()) {
                QtJson::JsonObject resultWindow;
                dump_object(window, resultWindow, with_properties);
                result[resultWindow["path"].toString()] = resultWindow;
            }
        }
    }
    return result;
}

QtJson::JsonObject Player::quit(const QtJson::JsonObject &) {
    if (qApp) {
        qApp->exit();
    }
    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::actions_list(const QtJson::JsonObject & command) {
    bool with_properties = command["with_properties"].toBool();
    QtJson::JsonObject result, resultAction;
    QList<QAction *> actions;
    if (command.contains("oid")) {
        ObjectLocatorContext ctx(this, command, "oid");
        if (ctx.hasError()) {
            return ctx.lastError;
        }
        foreach (QObject * obj, ctx.obj->children()) {
            actions += obj->findChildren<QAction *>();
        }
    } else {
        QList<QWidget *> widgets = QApplication::topLevelWidgets();
        if (!widgets.isEmpty()) {
            foreach (QWidget * widget, widgets) {
                actions += widget->findChildren<QAction *>();
            }
        }
    }

    foreach (QAction * action, actions) {
        dump_object(action, resultAction, with_properties);
        result[ObjectPath::objectName(action)] = resultAction;
    }

    return result;
}

QtJson::JsonObject Player::action_trigger(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QAction> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    bool blocking = command["blocking"].toBool();
    if (blocking) {
        // block until QAction::trigger() returns
        ctx.widget->trigger();
    } else {
        // trigger the action, but return immediately
        QTimer::singleShot(0, ctx.widget, SLOT(trigger()));
    }
    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::widget_click(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QWidget> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QString action = command["mouseAction"].toString();
    QPoint pos = ctx.widget->rect().center();
    if (action == "doubleclick") {
        mouse_dclick(ctx.widget, pos);
    } else if (action == "rightclick") {
        mouse_click(ctx.widget, pos, Qt::RightButton);
    } else if (action == "middleclick") {
        mouse_click(ctx.widget, pos, Qt::MiddleButton);
    } else {
        mouse_click(ctx.widget, pos, Qt::LeftButton);
    }
    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::quick_item_click(
    const QtJson::JsonObject & command) {
#ifdef QT_QUICK_LIB
#if QT_VERSION_MAJOR >= 6
    return createError(
        "Qt5Only", "This method is currently not supported with Qt6.");
#endif
    QuickItemLocatorContext ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }

    QPointF relativeCenter(ctx.item->width() / 2.0, ctx.item->height() / 2.0);

    QPoint sPos = ctx.item->mapToScene(relativeCenter).toPoint();

    mouse_click(ctx.window, sPos, Qt::LeftButton);
    QtJson::JsonObject result;
    return result;
#else
    Q_UNUSED(command);
    return createQtQuickOnlyError();
#endif
}

QtJson::JsonObject Player::widget_move(const QtJson::JsonObject & command) {
  WidgetLocatorContext<QWidget> ctx(this, command, "oid");
  if (ctx.hasError()) {
      return ctx.lastError;
  }

  QPoint pos = ctx.widget->pos();
  if (!command["x"].isNull()) {
    pos.setX(command["x"].toInt());
  }
  if (!command["y"].isNull()) {
    pos.setY(command["y"].toInt());
  }
  ctx.widget->move(pos);

  QtJson::JsonObject result;
  result["x"] = ctx.widget->x();
  result["y"] = ctx.widget->y();
  return result;
}

QtJson::JsonObject Player::widget_resize(const QtJson::JsonObject & command) {
  WidgetLocatorContext<QWidget> ctx(this, command, "oid");
  if (ctx.hasError()) {
      return ctx.lastError;
  }

  QSize size = ctx.widget->size();
  if (!command["width"].isNull()) {
    size.setWidth(command["width"].toInt());
  }
  if (!command["height"].isNull()) {
    size.setHeight(command["height"].toInt());
  }
  ctx.widget->resize(size);

  QtJson::JsonObject result;
  result["width"] = ctx.widget->width();
  result["height"] = ctx.widget->height();
  return result;
}

QtJson::JsonObject Player::widget_close(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QWidget> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }

    QTimer::singleShot(0, ctx.widget, SLOT(close()));

    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::widget_map_position(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QWidget> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QWidget * parent = 0;
    if (!command["parent_oid"].isNull()) {
        WidgetLocatorContext<QWidget> parentCtx(this, command, "parent_oid");
        if (parentCtx.hasError()) {
            return ctx.lastError;
        } else {
            parent = parentCtx.widget;
        }
    }
    QString direction = command["direction"].toString();
    QPoint pos;
    pos.setX(command["x"].toInt());
    pos.setY(command["y"].toInt());

    if (direction == "from") {
        if (parent) {
            pos = ctx.widget->mapFrom(parent, pos);
        } else {
            pos = ctx.widget->mapFromGlobal(pos);
        }
    } else if (direction == "to") {
        if (parent) {
            pos = ctx.widget->mapTo(parent, pos);
        } else {
            pos = ctx.widget->mapToGlobal(pos);
        }
    } else {
        return createError(
            "InvalidDirection",
            QString::fromUtf8("The direction '%1' is invalid").arg(direction));
    }

    QtJson::JsonObject result;
    result["x"] = pos.x();
    result["y"] = pos.y();
    return result;
}

QtJson::JsonObject Player::model(const QtJson::JsonObject & command) {
    ObjectLocatorContext ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }

    QAbstractItemModel * model = 0;
    if (QAbstractItemView * view = qobject_cast<QAbstractItemView *>(ctx.obj)) {
        model = view->model();
    } else if (QComboBox * cbx = qobject_cast<QComboBox *>(ctx.obj)) {
        model = cbx->model();
    }

    qulonglong modelId = registerObject(model);
    if (modelId != 0) {
        QtJson::JsonObject result;
        result["oid"] = modelId;
        dump_object(model, result);
        return result;
    } else {
        return createError(
            "MissingModel",
            QString("Unable to find model for object with id `%1`")
                .arg(ctx.id));
    }
}

QtJson::JsonObject Player::model_items(const QtJson::JsonObject & command) {
    ObjectLocatorContext ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }

    QAbstractItemModel * model = qobject_cast<QAbstractItemModel *>(ctx.obj);
    if (!model) {
        return createError(
            "NotAModel",
            QString("Object with id `%1` is not a QAbstractItemModel")
                .arg(ctx.id));
    }

    QtJson::JsonObject result;
    bool recursive = !(ctx.obj->inherits("QAbstractTableModel") ||
                       ctx.obj->inherits("QAbstractListModel"));
    dump_items_model(model, result, QModelIndex(), ctx.id, recursive);
    return result;
}

QtJson::JsonObject Player::model_item_action(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QAbstractItemView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QAbstractItemModel * model = ctx.widget->model();
    if (!model) {
        return createError(
            "MissingModel",
            QString::fromUtf8("The view (id:%1) has no associated model")
                .arg(ctx.id));
    }
    QModelIndex index =
        get_model_item(model, command["itempath"].toString(),
                       command["row"].toInt(), command["column"].toInt());
    if (!index.isValid()) {
        return createError(
            "MissingModelItem",
            QString::fromUtf8("Unable to find an item identified by %1")
                .arg(command["itempath"].toString()));
    }
    ctx.widget->scrollTo(index);  // item visible
    QString itemaction = command["itemaction"].toString();

    QPoint cursorPosition;

    if (itemaction == "click" || itemaction == "doubleclick") {
        QString origin = command["origin"].toString();
        int offsetX = command["offset_x"].toInt();
        int offsetY = command["offset_y"].toInt();
        QRect visualRect = ctx.widget->visualRect(index);
        cursorPosition = visualRect.center();
        if (origin == "left") {
            cursorPosition.setX(visualRect.x());
        } else if (origin == "right") {
            cursorPosition.setX(visualRect.width() - 1);
        }
        int newX = cursorPosition.x() + offsetX;
        int newY = cursorPosition.y() + offsetY;

        /* The new coordinates have to be within the bounds */
        if (newX < visualRect.x())
            newX = visualRect.x() + 2;
        else if (newX > visualRect.x() + visualRect.width())
            newX = visualRect.x() + visualRect.width() - 2;

        if (newY < visualRect.y())
            newY = visualRect.y() + 2;
        else if (newY > visualRect.y() + visualRect.height())
            newY = visualRect.y() + visualRect.height() - 2;

        cursorPosition.setX(newX);
        cursorPosition.setY(newY);
    }

    if (itemaction == "select") {
        _model_item_action(itemaction, ctx.widget, index);
    } else if (itemaction == "edit") {
        _model_item_action(itemaction, ctx.widget, index);
    } else if (itemaction == "click") {
        mouse_click(ctx.widget->viewport(), cursorPosition, Qt::LeftButton);
    } else if (itemaction == "rightclick") {
        mouse_click(ctx.widget->viewport(), cursorPosition, Qt::RightButton);
    } else if (itemaction == "middleclick") {
        mouse_click(ctx.widget->viewport(), cursorPosition, Qt::MiddleButton);
    } else if (itemaction == "doubleclick") {
        mouse_dclick(ctx.widget->viewport(), cursorPosition);
    } else {
        return createError(
            "MissingItemAction",
            QString::fromUtf8("itemaction %1 unknown").arg(itemaction));
    }
    QtJson::JsonObject result;
    return result;
}

void Player::_model_item_action(const QString & action,
                                QAbstractItemView * widget,
                                const QModelIndex & index) {
    if (action == "select") {
        widget->setCurrentIndex(index);
    } else if (action == "edit") {
        widget->setCurrentIndex(index);
        widget->edit(index);
    }
}

QtJson::JsonObject Player::model_gitem_action(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QGraphicsView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    qulonglong gid = command["gid"].value<qulonglong>();
    QGraphicsItem * item = graphicsItemFromId(ctx.widget, gid);
    if (!item) {
        return createError(
            "MissingGItem",
            QString::fromUtf8("The view (id:%1) has no associated item %2")
                .arg(ctx.id)
                .arg(gid));
    }
    ctx.widget->ensureVisible(item);  // be sure item is visible
    QString itemaction = command["itemaction"].toString();

    QPoint viewPos = ctx.widget->mapFromScene(
        item->mapToScene(item->boundingRect().center()));
    if (itemaction == "click" || itemaction == "rightclick" ||
        itemaction == "middleclick") {
        if (ctx.widget->scene() && ctx.widget->scene()->mouseGrabberItem()) {
            ctx.widget->scene()->mouseGrabberItem()->ungrabMouse();
        }
        if (itemaction == "rightclick") {
            mouse_click(ctx.widget->viewport(), viewPos, Qt::RightButton);
        } else if (itemaction == "middleclick") {
            mouse_click(ctx.widget->viewport(), viewPos, Qt::MiddleButton);
        } else {
            mouse_click(ctx.widget->viewport(), viewPos, Qt::LeftButton);
        }
    } else if (itemaction == "doubleclick") {
        if (ctx.widget->scene() && ctx.widget->scene()->mouseGrabberItem()) {
            ctx.widget->scene()->mouseGrabberItem()->ungrabMouse();
        }
        mouse_dclick(ctx.widget->viewport(), viewPos);
    } else {
        return createError(
            "MissingItemAction",
            QString::fromUtf8("itemaction %1 unknown").arg(itemaction));
    }
    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::grab(const QtJson::JsonObject & command) {
    QPixmap pixmap;
    if (command.contains("oid")) {
        // grab a single widget
        WidgetLocatorContext<QWidget> ctx(this, command, "oid");
        if (ctx.hasError()) {
            return ctx.lastError;
        }
#if QT_VERSION_MAJOR >= 6
        pixmap = ctx.widget->grab();
#else
        pixmap = QPixmap::grabWidget(ctx.widget);
#endif
    } else {
        // grab the whole screen
#if QT_VERSION_MAJOR >= 6
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            pixmap = screen->grabWindow();
        }
#else
        pixmap = QPixmap::grabWindow(QApplication::desktop()->winId());
#endif
    }
    QString format = command["format"].toString();
    if (format.isEmpty()) {
        format = "PNG";
    }

    QBuffer buffer;
    pixmap.save(&buffer, "PNG");

    QtJson::JsonObject result;
    result["format"] = format;
    result["data"] = buffer.data().toBase64();
    return result;
}

QtJson::JsonObject Player::widget_keyclick(const QtJson::JsonObject & command) {
    QWidget * widget;
    if (command.contains("oid")) {
        WidgetLocatorContext<QWidget> ctx(this, command, "oid");
        if (ctx.hasError()) {
            return ctx.lastError;
        }
        widget = ctx.widget;
    } else {
        widget = qApp->activeWindow();
    }
    QString text = command["text"].toString();
    for (int i = 0; i < text.count(); ++i) {
        QChar ch = text[i];
        int key = (int)ch.toLatin1();
        qApp->postEvent(
            widget,
            new QKeyEvent(QKeyEvent::KeyPress, key, Qt::NoModifier, ch));
        qApp->postEvent(
            widget,
            new QKeyEvent(QKeyEvent::KeyRelease, key, Qt::NoModifier, ch));
    }
    QtJson::JsonObject result;
    return result;
}

DelayedResponse * Player::shortcut(const QtJson::JsonObject & command) {
    return new ShortcutResponse(this, command);
}

QtJson::JsonObject Player::tabbar_list(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QTabBar> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QStringList texts;
    for (int i = 0; i < ctx.widget->count(); ++i) {
        texts << ctx.widget->tabText(i);
    }
    QtJson::JsonObject result;
    result["tabtexts"] = texts;
    return result;
}

QtJson::JsonObject Player::headerview_list(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QHeaderView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QAbstractItemModel * model = ctx.widget->model();
    if (!model) {
        return createError(
            "MissingModel",
            QString::fromUtf8("The header view (id:%1) has no associated model")
                .arg(ctx.id));
    }
    QStringList texts;
    int nbItems = ctx.widget->orientation() == Qt::Vertical
        ? model->rowCount()
        : model->columnCount();
    for (int i = 0; i < nbItems; i++) {
        texts << model->headerData(i, ctx.widget->orientation()).toString();
    }
    QtJson::JsonObject result;
    result["headertexts"] = texts;
    return result;
}

QtJson::JsonObject Player::headerview_click(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QHeaderView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    int logicalIndex;
    QVariant indexOrName = command["indexOrName"];
    if (indexOrName.type() == QVariant::String) {
        QString name = indexOrName.toString();
        QAbstractItemModel * model = ctx.widget->model();
        if (!model) {
            return createError(
                "MissingModel",
                QString::fromUtf8(
                    "The header view (id:%1) has no associated model")
                    .arg(ctx.id));
        }
        bool found = false;
        int nbItems = ctx.widget->orientation() == Qt::Horizontal
            ? model->rowCount()
            : model->columnCount();
        for (int i = 0; i < nbItems; i++) {
            if (name ==
                model->headerData(i, ctx.widget->orientation()).toString()) {
                logicalIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            return createError(
                "MissingHeaderViewText",
                QString::fromUtf8(
                    "The header view (id:%1) has no text column `%2`")
                    .arg(ctx.id)
                    .arg(name));
        }
    } else {
        logicalIndex = ctx.widget->logicalIndex(command["indexOrName"].toInt());
    }

    int pos = ctx.widget->sectionPosition(logicalIndex);
    if (pos == -1) {
        return createError(
            "InvalidHeaderViewIndex",
            QString::fromUtf8(
                "The header view (id:%1) has no index %2 or it is hidden")
                .arg(ctx.id)
                .arg(logicalIndex));
    }
    QPoint mousePos;
    if (ctx.widget->orientation() == Qt::Horizontal) {
        mousePos.setY(ctx.widget->height() / 2);
        mousePos.setX(pos + ctx.widget->offset() + 5);
    } else {
        mousePos.setX(ctx.widget->width() / 2);
        mousePos.setY(pos + ctx.widget->offset() + 5);
    }
    mouse_click(ctx.widget->viewport(), mousePos, Qt::LeftButton);
    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::headerview_path_from_view(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QAbstractItemView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }

    QHeaderView * header = NULL;
    QTableView * tview = qobject_cast<QTableView *>(ctx.widget);
    if (tview) {
        if (command["orientation"] == "vertical") {
            header = tview->verticalHeader();
        } else {
            header = tview->horizontalHeader();
        }
    } else {
        QTreeView * trview = qobject_cast<QTreeView *>(ctx.widget);
        if (trview) {
            header = trview->header();
        }
    }

    if (!header) {
        return createError(
            "InvalidHeaderView",
            QString::fromUtf8("No header view found for the view (id:%1)")
                .arg(ctx.id));
    }
    QtJson::JsonObject result;
    result["headerpath"] = objectPath(header);
    return result;
}

QtJson::JsonObject Player::graphicsitems(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QGraphicsView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QList<QGraphicsItem *> topLevelItems;
    foreach (QGraphicsItem * item, ctx.widget->items()) {
        if (!item->parentItem()) {
            topLevelItems << item;
        }
    }
    QtJson::JsonObject result;
    dump_graphics_items(topLevelItems, ctx.id, result);
    return result;
}

QtJson::JsonObject Player::gitem_properties(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QGraphicsView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    qulonglong gid = command["gid"].value<qulonglong>();
    QGraphicsItem * item = graphicsItemFromId(ctx.widget, gid);
    if (!item) {
        return createError(
            "MissingGItem",
            QString::fromUtf8("QGraphicsitem %1 is not in view %2")
                .arg(gid)
                .arg(ctx.id));
    }
    QObject * object = dynamic_cast<QObject *>(item);
    if (!object) {
        return createError(
            "GItemNotQObject",
            QString::fromUtf8(
                "QGraphicsitem %1 in view %2 does not inherit from QObject")
                .arg(gid)
                .arg(ctx.id));
    }
    QtJson::JsonObject result;
    dump_properties(object, result);
    return result;
}

DelayedResponse * Player::drag_n_drop(const QtJson::JsonObject & command) {
    return new DragNDropResponse(this, command);
}

QtJson::JsonObject Player::call_slot(const QtJson::JsonObject & command) {
    WidgetLocatorContext<QWidget> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QString slot_name = command["slot_name"].toString();
    QVariant result_slot;
    bool invokedMeth = QMetaObject::invokeMethod(
        ctx.widget, slot_name.toLocal8Bit().data(), Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, result_slot),
        Q_ARG(QVariant, command["params"]));
    if (!invokedMeth) {
        return createError("NoMethodInvoked",
                           QString::fromUtf8("The slot %1 could not be called")
                               .arg(slot_name));
    }

    QtJson::JsonObject result;
    result["result_slot"] = result_slot;
    return result;
}

QtJson::JsonObject Player::widget_activate_focus(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QWidget> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    activate_focus(ctx.widget);

    QtJson::JsonObject result;
    return result;
}

QtJson::JsonObject Player::grab_graphics_view(
    const QtJson::JsonObject & command) {
    WidgetLocatorContext<QGraphicsView> ctx(this, command, "oid");
    if (ctx.hasError()) {
        return ctx.lastError;
    }
    QString format = command["format"].toString();
    if (format.isEmpty()) {
        format = "PNG";
    }
    QPixmap pixmap(ctx.widget->scene()->width(), ctx.widget->scene()->height());
    QPainter q_painter(&pixmap);

    ctx.widget->scene()->render(&q_painter);
    QBuffer buffer;
    pixmap.save(&buffer, format.toStdString().c_str());

    QtJson::JsonObject result;
    result["format"] = format;
    result["data"] = buffer.data().toBase64();

    return result;
}
