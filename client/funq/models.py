# -*- coding: utf-8 -*-

# Copyright: SCLE SFE
# Contributor: Julien Pagès <j.parkouss@gmail.com>
#
# This software is a computer program whose purpose is to test graphical
# applications written with the QT framework (http://qt.digia.com/).
#
# This software is governed by the CeCILL v2.1 license under French law and
# abiding by the rules of distribution of free software.  You can  use,
# modify and/ or redistribute the software under the terms of the CeCILL
# license as circulated by CEA, CNRS and INRIA at the following URL
# "http://www.cecill.info".
#
# As a counterpart to the access to the source code and  rights to copy,
# modify and redistribute granted by the license, users are provided only
# with a limited warranty  and the software's author,  the holder of the
# economic rights,  and the successive licensors  have only  limited
# liability.
#
# In this respect, the user's attention is drawn to the risks associated
# with loading,  using,  modifying and/or developing or reproducing the
# software by the user in light of its specific status of free software,
# that may mean  that it is complicated to manipulate,  and  that  also
# therefore means  that it is reserved for developers  and  experienced
# professionals having in-depth computer knowledge. Users are therefore
# encouraged to load and test the software's suitability as regards their
# requirements in conditions enabling the security of their systems and/or
# data to be ensured and,  more generally, to use and operate it in the
# same conditions as regards security.
#
# The fact that you are presently reading this means that you have had
# knowledge of the CeCILL v2.1 license and that you accept its terms.

"""
Definition of widgets and models useable in funq.
"""
from funq.tools import wait_for
from funq.errors import FunqError
import json
import base64


class TreeItem():  # pylint: disable=R0903
    """
    Defines an abstract item that contains subitems
    """

    def __init__(self, client, data):
        """
        Allow to create a TreeItem from a dico data decoded from json.
        """
        self.client = client

        for k, v in data.items():
            setattr(self, k, v)
        self.items = [self.__class__(client, d) for d in getattr(self, 'items', [])]


class BaseItems():
    """ Class to handle 'item_class' argument for derived subclasses """

    def __init_subclass__(cls, /, item_class, **kwargs):
        super().__init_subclass__(**kwargs)
        cls._item_class = item_class


class TreeItems(BaseItems, item_class=TreeItem):
    """
    Abstract class to manipulate data that contains :class:`TreeItem`. Used
    by modelitems and graphicsitems.
    """

    def __init__(self, client, data):
        """
        Allow to create an instance of the class given some data coming from
        decoded json.
        """
        self.client = client
        self.items = [self._item_class(client, v) for v in data['items']]

    def iter(self):
        """
        Allow to iterate on every item recursively. Kept for backward
        compatibility. Iterate over the class instance itself instead.

        Example::

          for item in items.iter():
              print(item)
        """

        return iter(self)

    def __iter__(self):
        """
        Allow to iterate on every item recursively.

        Example::

          for item in items:
              print(item)
        """

        def __iter(items):
            for item in items:
                yield item
                yield from __iter(item.items)

        yield from __iter(self.items)


class WidgetMetaClass(type):
    """
    Saves a dict of accessible classes to handle inheritance of Widgets.
    """
    _cpp_classes = {}

    def __new__(mcs, name, bases, attrs, /, cpp_class=None):
        cls = super().__new__(mcs, name, bases, attrs)
        if cpp_class:
            mcs._cpp_classes[cpp_class] = cls
        return cls

    def __call__(cls, client, data):
        for cppcls in data.get('classes', []):
            if cppcls in cls._cpp_classes:
                if cls is cls._cpp_classes[cppcls]:  # prevent infinite recursion
                    break
                return cls._cpp_classes[cppcls](client, data)
        return super().__call__(client, data)


class Object(metaclass=WidgetMetaClass):
    """
    Allow to manipulate a QObject or derived.

    :var client: client for the communication with libFunq
                 [type: :class:`funq.client.FunqClient`]
    :var oid: ID of the managed C++ instance. [type: long]
    :var path: complete path to the object [type: str]
    :var classes: list of class names of the managed C++ instance,
                  in inheritance order (ie 'QObject' is last)
                  [type : list(str)]
    """

    oid = None
    client = None
    path = None

    class Properties():
        def __init__(self, obj):
            super().__setattr__(f'_{self.__class__.__name__}__obj', obj)

        def __call__(self):
            """
            Returns a dict of availables properties for this object with associated
            values.

            Example::

              enabled = object.properties()["enabled"]
            """
            return self.__obj.client.send_command('object_properties', oid=self.__obj.oid)

        def __contains__(self, item):
            return item in self()

        def __getitem__(self, item):
            """
            Get one property from object.

            Example::

              value = object.property['text']
            """
            return self()[item]

        def __setitem__(self, item, value):
            """
            Define one property on object.

            Example::

              object.property['text'] = "My beautiful text"
            """
            self.__obj.set_properties(**{item: value})

        def __getattr__(self, item):
            """
            Get value of one property from object.

            Example::

              value = object.property.text
            """
            return self()[item]

        def __setattr__(self, item, value):
            """
            Define one property on object.

            Example::

              object.property.text = "My beautiful text"
            """
            self.__obj.set_properties(**{item: value})

    def __init__(self, client, data):
        """
        Allow to create an Object or a subclass given data coming from
        decoded json.
        """
        for k, v in data.items():
            setattr(self, k, v)
        self.client = client
        self.property = self.properties = self.Properties(self)

    def set_properties(self, **properties):
        """
        Define some properties on this object.

        Example::

          object.set_properties(text="My beautiful text")
        """
        self.client.send_command('object_set_properties',
                                 oid=self.oid,
                                 properties=properties)

    def set_property(self, name, value):
        """
        Define one property on this object.

        Example::

          object.set_property('text', "My beautiful text")
        """
        self.set_properties(**{name: value})

    def wait_for_properties(self, props, timeout=10.0, timeout_interval=0.1):
        """
        Wait for the properties to have the given values.

        Example::

          self.wait_for_properties({'enabled': True, 'visible': True})
        """
        def check_props():
            properties = self.properties()
            for k, v in props.items():
                if properties.get(k) != v:
                    return False
            return True
        return wait_for(check_props, timeout, timeout_interval)

    def call_slot(self, slot_name, params=None):
        """
        **CAUTION**; This methods allows to call a slot (written on the tested
        application). The slot must take a QVariant and returns a QVariant.

        This is not really recommended to use this method, as it will trigger
        code in the tested application in an unusual way.

        The methods returns what the slot returned, decoded as python object.

        :param slot_name: name of the slot
        :param params: parameters (must be json serialisable) that will be send
                       to the tested application as a QVariant.
        """
        return self.client.send_command(
            'call_slot',
            slot_name=slot_name,
            params={} if params is None else params,
            oid=self.oid
        )['result_slot']


class Action(Object):
    """
    Allow to manipulate a QAction or derived.
    """

    def trigger(self, blocking=True, wait_for_enabled=10.0):
        """
        Trigger the QAction. If wait_for_enabled is > 0 (default), it will wait
        until the action becomes active (enabled and visible) before triggering
        it. If blocking is True (default), funq waits until the triggered
        action has completed (synchronous trigger). For actions which are
        blocking by themself (e.g. actions which open a modal dialog), you must
        set blocking to False (asynchronous trigger), otherwise funq freezes.
        """
        if wait_for_enabled > 0.0:
            self.wait_for_properties({'enabled': True, 'visible': True},
                                     timeout=wait_for_enabled)
        self.client.send_command('action_trigger', oid=self.oid,
                                 blocking=blocking)


class AbstractItemModel(Object):
    """
    Allow to manipulate a QAbstractItemModel or derived.
    """

    def items(self):
        """
        Returns an instance of :class:`ModelItems` with all items of this
        model.
        """
        data = self.client.send_command('model_items', oid=self.oid)
        return ModelItems(self.client, data)


class Widget(Object):
    """
    Allow to manipulate a QWidget or derived.
    """

    def click(self, wait_for_enabled=10.0, btn='left'):
        """
        Click on the widget.

        If wait_for_enabled is > 0 (default), it will wait until the widget
        become active (enabled and visible) before sending click.

        The target mouse button can be configured with the `btn` parameter: It
        can be either 'left', 'middle' or 'right'. Default is 'left'.

        """
        if wait_for_enabled > 0.0:
            self.wait_for_properties({'enabled': True, 'visible': True},
                                     timeout=wait_for_enabled)
        if btn == 'left':
            action = 'click'
        elif btn == 'right':
            action = 'rightclick'
        elif btn == 'middle':
            action = 'middleclick'
        else:
            raise ValueError(f'Invalid mouse button: {btn}')
        self.client.send_command('widget_click',
                                 oid=self.oid,
                                 mouseAction=action)

    def dclick(self, wait_for_enabled=10.0):
        """
        Double click on the widget. If wait_for_enabled is > 0 (default), it
        will wait until the widget become active (enabled and visible) before
        sending click.
        """
        if wait_for_enabled > 0.0:
            self.wait_for_properties({'enabled': True, 'visible': True},
                                     timeout=wait_for_enabled)
        self.client.send_command('widget_click',
                                 oid=self.oid,
                                 mouseAction='doubleclick')

    def activate_focus(self):
        """
        Activate the widget then set focus on.
        """
        self.client.send_command('widget_activate_focus', oid=self.oid)

    def keyclick(self, text):
        """
        Simulate keypress and keyrelease events for every character in the
        given text. Example::

          widget.keyclick("my text")
        """
        self.client.send_command('widget_keyclick', text=text, oid=self.oid)

    def shortcut(self, key_sequence):
        """
        Send a shortcut on the widget, defined with a text sequence. See the
        QKeySequence::fromString to see the documentation of the format needed
        for the text sequence.

        :param text: text sequence of the shortcut (see
                     QKeySequence::fromString documentation)
        """
        self.client.send_command('shortcut',
                                 keysequence=key_sequence,
                                 oid=self.oid)

    def drag_n_drop(self, src_pos=None,
                    dest_widget=None, dest_pos=None):
        """
        Do a drag and drop from this widget.

        :param src_pos: starting position of the drag. Must be a tuple (x, y)
                        in widget coordinates or None (the center of the widget
                        will then be used)
        :param dest_widget: destination widget. If None, src_widget will be
                            used.
        :param dest_pos: ending position (the drop). Must be a tuple (x, y)
                         in widget coordinates or None (the center of the dest
                         widget will then be used)
        """
        self.client.drag_n_drop(self, src_pos=src_pos, dest_widget=dest_widget,
                                dest_pos=dest_pos)

    def move(self, x=None, y=None):
        """
        Move the widget, using QWidget::move(). One can either only change X or
        Y coordinate, or both at the same time.

        :param int x: New X coordinate (optional).
        :param int y: New Y coordinate (optional).
        :return: New position as tuple (X, Y).
        """
        response = self.client.send_command('widget_move', oid=self.oid,
                                            x=x, y=y)
        return response['x'], response['y']

    def resize(self, width=None, height=None):
        """
        Resize the widget, using QWidget::resize(). One can either only change
        width or height, or both at the same time.

        :param int width: New width (optional).
        :param int height: New height (optional).
        :return: New size as tuple (width, height).
        """
        response = self.client.send_command('widget_resize', oid=self.oid,
                                            width=width, height=height)
        return response['width'], response['height']

    def close(self):
        """
        Ask to close a widget, using QWidget::close().
        """
        self.client.send_command('widget_close', oid=self.oid)

    def grab(self, format_="PNG"):
        """
        Save the widgets content as an image.

        :param string format: The format of the grabbed image.
        :return: The image as a binary blob in the given format.
        """
        data = self.client.send_command('grab', format=format_, oid=self.oid)
        return base64.standard_b64decode(data['data'])

    def map_position_from(self, x, y, parent):
        """
        Map a given parent's coordinate or global coordinate to a local widget
        coordinate. See Qt's documentation of `QWidget::mapFrom()` and
        `QWidget::mapFromGlobal()` for details.

        :param int x: X coordinate relative to parent.
        :param int y: Y coordinate relative to parent.
        :param parent: One of this widget's parent (:class:`ModelItem`)
                       or `None` to map from global coordinates.
        :return: Local widget coordinates as tuple (X, Y).
        """
        parent_oid = parent.oid if parent else None
        response = self.client.send_command('widget_map_position',
                                            oid=self.oid,
                                            parent_oid=parent_oid,
                                            direction='from', x=x, y=y)
        return response['x'], response['y']

    def map_position_to(self, x, y, parent):
        """
        Map a given widget coordinate to a parent's coordinate or global
        coordinate. See Qt's documentation of `QWidget::mapTo()` and
        `QWidget::mapToGlobal()` for details.

        :param int x: Local X coordinate.
        :param int y: Local Y coordinate.
        :param parent: One of this widget's parent ([type: :class:`ModelItem`])
                       or None to map to global coordinates.
        :return: Coordinates relative to the given parent widget, or
                 global coordinates as tuple (X, Y).
        """
        parent_oid = parent.oid if parent else None
        response = self.client.send_command('widget_map_position',
                                            oid=self.oid,
                                            parent_oid=parent_oid,
                                            direction='to', x=x, y=y)
        return response['x'], response['y']

    def actions_list(self, with_properties=False):
        """
        Returns a dict with every actions starting with widget.
        """
        return self.client.send_command('actions_list',
                                        oid=self.oid,
                                        with_properties=with_properties)

    def widgets_list(self, with_properties=False, recursive=True):
        """
        Returns a dict with children or every sub-widgets of the widget.
        """
        return self.client.send_command('widgets_list',
                                        oid=self.oid,
                                        with_properties=with_properties,
                                        recursive=recursive)


class ModelItem(TreeItem):
    """
    Allow to manipulate a modelitem in a QAbstractItemModel or derived.

    :var modelid: ID of the model containing this item [type: long]
    :var row: item row number [type: int]
    :var column: item column number [type: int]
    :var value: item text value [type: unicode]
    :var check_state: item text value of the check state, or None
    :var itempath: Internal ID to localize this item [type: str ou None]
    :var items: list of subitems [type: :class:`ModelItem`]
    """

    modelid = None
    row = None
    column = None
    itempath = None
    check_state = None

    def is_checkable(self):
        """Returns True if the item is checkable"""
        return self.check_state is not None

    def is_checked(self):
        """Returns True if the item is checked"""
        return self.check_state == 'checked'


class ModelItems(TreeItems, item_class=ModelItem):
    """
    Allow to manipulate all modelitems in a QAbstractItemModel or derived.

    :var items: list of :class:`ModelItem`
    """

    def item_by_named_path(self, named_path, match_column=0, sep='/',
                           column=0):
        """
        Returns the item (:class:`ModelItem`) that match the arborescence
        defined by `named_path` and in the given column.

        .. note::

          The arguments are the same as for :meth:`row_by_named_path`, with
          the addition of `column`.

        :param column: the column of the desired item
        """
        items = self.row_by_named_path(named_path,
                                       match_column=match_column,
                                       sep=sep)
        return items[column] if items else None

    def row_by_named_path(self, named_path, match_column=0, sep='/'):
        """
        Returns the item list of :class:`ModelItem` that match the arborescence
        defined by `named_path`, or None if the path does not exists.

        .. important::

          Use unicode characters in `named_path` to match elements with
          non-ascii characters.

        Example::

          model_items.row_by_named_path(['TG/CBO/AP (AUT 1)',
                                         'Paramètres tranche',
                                         'TG',
                                         'DANGER'])

        :param named_path: path for the interesting ModelIndex. May be
                           defined with a list of str or with a single str
                           that will be splitted on `sep`.
        :param match_column: column used to check`named_path` is a string.
        """
        if isinstance(named_path, (list, tuple)):
            parts = list(named_path)
        else:
            parts = named_path.split(sep)
        item = self
        while item and parts:
            next_item = None
            part = parts.pop(0)
            for item_ in item.items:
                if match_column == item_.column and item_.value == part:
                    # we found the item
                    # if it is the last part name, just return the columns
                    if not parts:
                        row = [it for it in item.items
                               if it.row == item_.row]
                        return sorted(row, key=lambda it: it.column)
                    next_item = item_
            item = next_item
        return None


class AbstractItemView(Widget, cpp_class='QAbstractItemView'):
    """
    Specific Widget to manipulate QAbstractItemView or derived.
    """
    editor_class_names = ('QLineEdit', 'QComboBox', 'QSpinBox',
                          'QDoubleSpinBox')

    def model(self):
        """
        Returns the model (:class:`AbstractItemModel`) which is displayed in
        this item view widget.
        """
        data = self.client.send_command('model', oid=self.oid)
        return AbstractItemModel(self.client, data)

    def _item_action(self, item, itemaction, origin=None, offset_x=None,
                     offset_y=None):
        """ Send the 'model_item_action' action for a given item """
        self.client.send_command('model_item_action',
                                 oid=self.oid,
                                 itemaction=itemaction,
                                 row=item.row, column=item.column,
                                 origin=origin,
                                 offset_x=offset_x,
                                 offset_y=offset_y,
                                 itempath=item.itempath)

    def open_context_menu_item(self, item):
        """
        Open context menu for the specified item.

        :param ModelItem item: The item which context menu need to open (object retrieved from
                               (:meth:`model`)).
        """
        self._item_action(item, "context_menu")

    def select_item(self, item):
        """
        Select the specified item.

        :param ModelItem item: The item to select (object retrieved from
                               (:meth:`model`)).
        """
        self._item_action(item, "select")

    def edit_item(self, item):
        """
        Edit the specified item.

        :param ModelItem item: The item to edit (object retrieved from
                               (:meth:`model`)).
        """
        self._item_action(item, "edit")

    def click_item(self, item, origin="center", offset_x=0, offset_y=0,
                   btn="left"):
        """
        Click on the specified item.

        :param ModelItem item: The item to click (object retrieved from
                               (:meth:`model`)).
        :param origin: Origin of the cursor coordinates of the ModelItem
                       object. Availables values: "center", "left" or "right".
        :param offset_x: x position relative to the origin.
                         Negative value allowed.
        :param offset_y: y position relative to the origin.
                         Negative value allowed.
        :param btn: The mouse button to click.
                    Available values: "left", "middle" or "right".
        """
        if btn == "left":
            action = "click"
        elif btn == "right":
            action = "rightclick"
        elif btn == "middle":
            action = "middleclick"
        else:
            raise ValueError(f"Invalid mouse button: {btn}")
        self._item_action(
            item, action, origin=origin, offset_x=offset_x, offset_y=offset_y
        )

    def dclick_item(self, item, origin="center", offset_x=0, offset_y=0):
        """
        Double click on the specified item.

        :param ModelItem item: The item to click (object retrieved from
                               (:meth:`model`)).
        :param origin: Origin of the cursor coordinates of the ModelItem
                       object.
        :param offset_x: x position relative to the origin.
                         Negative value allowed.
        :param offset_y: y position relative to the origin.
                         Negative value allowed.
        """
        self._item_action(
            item, "doubleclick", origin=origin, offset_x=offset_x,
            offset_y=offset_y
        )

    def current_editor(self, editor_class_name=None):
        """
        Returns the editor actually opened on this view. One item must be
        in editing mode, by using :meth:`ModelItem.dclick` or
        :meth:`ModelItem.edit` for example.

        Currently these editor types are handled:
        'QLineEdit', 'QComboBox', 'QSpinBox' and 'QDoubleSpinBox'.

        :param editor_class_name: name of the editor type. If None, every
                                  type of editor will be tested (this may
                                  actually be very slow)
        """
        qt_path = '::qt_scrollarea_viewport::'
        if editor_class_name:
            return self.client.widget(path=self.path
                                      + qt_path + editor_class_name)

        for editor_class_name in self.editor_class_names:
            try:
                return self.client.widget(path=self.path
                                          + qt_path + editor_class_name)
            except FunqError:
                pass

        raise FunqError("MissingEditor", 'Unable to find an editor.'
                        f' Possible editors: {repr(self.editor_class_names)}')


class TableView(AbstractItemView, cpp_class='QTableView'):
    """
    Specific widget to manipulate a QTableView widget.
    """

    def vertical_header(self,
                        timeout=2.0, timeout_interval=0.1, wait_active=True):
        """
        Return the vertical :class:`HeaderView` associated to this
        tableview.

        Each optionnal parameter is passed to
        :meth:`funq.client.FunqClient.widget`.
        """
        headerdata = self.client.send_command('headerview_path_from_view',
                                              oid=self.oid,
                                              orientation='vertical')
        return self.client.widget(path=headerdata['headerpath'],
                                  timeout=timeout,
                                  timeout_interval=timeout_interval,
                                  wait_active=wait_active)

    def horizontal_header(self,
                          timeout=2.0, timeout_interval=0.1, wait_active=True):
        """
        Return the horizontal :class:`HeaderView` associated to this
        tableview.

        Each optionnal parameter is passed to
        :meth:`funq.client.FunqClient.widget`.
        """
        headerdata = self.client.send_command('headerview_path_from_view',
                                              oid=self.oid,
                                              orientation='horizontal')
        return self.client.widget(path=headerdata['headerpath'],
                                  timeout=timeout,
                                  timeout_interval=timeout_interval,
                                  wait_active=wait_active)


class TreeView(AbstractItemView, cpp_class='QTreeView'):
    """
    Specific widget to manipulate a QTreeView widget.
    """

    def header(self, timeout=2.0, timeout_interval=0.1, wait_active=True):
        """
        Return the :class:`HeaderView` associated to this treeview.

        Each optionnal parameter is passed to
        :meth:`funq.client.FunqClient.widget`.
        """
        headerdata = self.client.send_command('headerview_path_from_view',
                                              oid=self.oid)
        return self.client.widget(path=headerdata['headerpath'],
                                  timeout=timeout,
                                  timeout_interval=timeout_interval,
                                  wait_active=wait_active)


class TabBar(Widget, cpp_class="QTabBar"):
    """
    Allow to manipulate a QTabBar Widget.
    """

    def tab_texts(self):
        """
        Returns the list of texts in tabbar.
        """
        data = self.client.send_command('tabbar_list', oid=self.oid)
        return data["tabtexts"]

    def set_current_tab(self, tab_index_or_name):
        """
        Define the current tab given an index or a tab text.
        """
        tabnames = self.tab_texts()
        if isinstance(tab_index_or_name, int):
            index = tab_index_or_name
            if index < 0 or index >= len(tabnames):
                raise ValueError(f"Invalid tab Index {index}")
        else:
            index = tabnames.index(tab_index_or_name)
        self.set_property('currentIndex', index)


class GItem(TreeItem):
    """
    Allow to manipulate a QGraphicsItem.

    :var viewid: ID of the view attached to the model containing this item
                 [type: long]
    :var gid: Internal gitem ID [type: unsigned long long]
    :var objectname: value of the "objectName" property if it inherits
                     from QObject. [type: unicode or None]
    :var classes: list of names of class inheritance if it inherits from
                  QObject. [type: list(str) or None]
    :var items: list of subitems [type: :class:`GItem`]
    """
    viewid = None
    gid = None
    objectname = None
    classes = None

    def is_qobject(self):
        """ Returns True if this GItem inherits QObject """
        return self.objectname is not None

    def properties(self):
        """
        Return the properties of the GItem. The GItem must inherits from
        QObject.
        """
        return self.client.send_command('gitem_properties',
                                        oid=self.viewid,
                                        gid=self.gid)

    def _action(self, itemaction):
        """ Send the command 'model_gitem_action' """
        self.client.send_command('model_gitem_action',
                                 oid=self.viewid,
                                 itemaction=itemaction,
                                 gid=self.gid)

    def click(self, btn="left"):
        """
        Click on this gitem.
        """
        if btn == "left":
            action = "click"
        elif btn == "right":
            action = "rightclick"
        elif btn == "middle":
            action = "middleclick"
        else:
            raise ValueError(f"Invalid mouse button: {btn}")
        self._action(action)

    def dclick(self):
        """
        Double click on this gitem.
        """
        self._action("doubleclick")


class GItems(TreeItems, item_class=GItem):

    """
    Allow to manipulate a group of QGraphicsItems.

    :var items: list of :class:`GItem` that are on top of the scene
                (and not subitems)
    """


class GraphicsView(Widget, cpp_class='QGraphicsView'):
    """
    Allow to manipulate an instance of QGraphicsView.
    """

    def gitems(self):
        """
        Returns an instance of :class:`GItems`, that will contains every items
        of this QGraphicsView.
        """
        data = self.client.send_command('graphicsitems', oid=self.oid)
        return GItems(self.client, data)

    def dump_gitems(self, stream='gitems.json'):
        """
        Write in a file the list of graphics items.
        """
        data = self.client.send_command('graphicsitems', oid=self.oid)
        if isinstance(stream, str):
            stream = open(stream, 'w')
        json.dump(data,
                  stream, sort_keys=True, indent=4, separators=(',', ': '))

    def grab_scene(self, stream, format_="PNG"):
        """
        Save the full QGraphicsScene content under the GraphicsView as an
        image.

        .. versionadded:: 1.2.0
        """
        data = self.client.send_command('grab_graphics_view', format=format_,
                                        oid=self.oid)
        has_to_be_closed = False
        if isinstance(stream, str):
            stream = open(stream, 'wb')
            has_to_be_closed = True
        raw = base64.standard_b64decode(data['data'])
        stream.write(raw)
        if has_to_be_closed:
            stream.close()


class ComboBox(Widget, cpp_class='QComboBox'):
    """
    Allow to manipulate a QCombobox.
    """

    def model(self):
        """
        Returns the model (:class:`AbstractItemModel`) which contains the items
        of this combobox.
        """
        data = self.client.send_command('model', oid=self.oid)
        return AbstractItemModel(self.client, data)

    def set_current_text(self, text):
        """
        Define the text of the combobox, ensuring that it is a possible value.
        """
        if not isinstance(text, str):
            raise TypeError('the text parameter must be a string'
                            f' - got {type(text)}')
        column = self.properties()['modelColumn']
        index = -1
        for item in self.model().items():
            if column == item.column and item.value == text:
                index = item.row
                break
        assert index > -1, (f"The text `{text}` is not in the combobox `{self.path}`")
        self.set_property('currentIndex', index)


class HeaderView(Widget, cpp_class='QHeaderView'):
    """
    Allow to manipulate a QHeaderView.
    """

    def header_texts(self):
        """
        Returns the list of texts in the headerview.
        """
        data = self.client.send_command('headerview_list', oid=self.oid)
        return data["headertexts"]

    def header_click(self, index_or_name):
        """
        Click on the given header, identified by a visual index or
        a displayed name.
        """
        return self.client.send_command('headerview_click',
                                        oid=self.oid,
                                        indexOrName=index_or_name)


class QuickItem(Object, cpp_class="QQuickItem"):
    """
    Represent a QQuickItem or derived.

    You can get a :class:`QuickItem` instance by using
    :meth:`QuickWindow.item`.
    """

    def click(self):
        """
        Click on the :class:`QuickItem`.
        """
        self.client.send_command(
            "quick_item_click",
            oid=self.oid
        )


class QuickWindow(Widget, cpp_class="QQuickWindow"):
    """
    Represent a QQuickWindow or QQuickView.

    If your application is a qml app with one window, you can easily get the
    :class:`QuickWindow` with :meth:`funq.FunqClient.active_widget`.

    Example::

      quick_window = self.funq.active_widget()
    """

    def item(self, alias=None, path=None, id=None):
        """
        Search for a :class:`funq.models.QuickItem` and returns it.

        An item can be identified by its id, using an alias or using a
        raw path. The preferred way is using an id (defined in the qml
        file) - this takes precedence over other methods.

        For example, with the following qml code:

        .. code-block:: qml

          import QtQuick 2.0

          Item {
            id: root
            width: 320
            height: 480

            Rectangle {
                id: rect
                color: "#272822"
                width: 320
                height: 480
            }
          }

        You can use the following statements::

          # using id
          root = quick_window.item(id='root')
          rect = quick_window.item(id='root.rect') # or just 'rect'

          # using alias
          # you must have something like the following line in your alias file:
          # my_rect = QQuickView::QQuickItem::QQuickRectangle
          rect = quick_window.item(alias='my_rect')

          # using raw path - note the path is relative to the quick window
          rect = quick_window.item(path='QQuickItem::QQuickRectangle')

        :param alias: alias defined in the aliases file that points to the
                      item.
        :param path: path of the item, relative to the view (do not pass
                     the full path)
        :param id: id of the qml item.
        """
        if not (alias or path or id):
            raise TypeError("alias, path or id must be defined")

        if alias and not id:
            path = self.client.aliases[alias]
            if not path.startswith(self.path):
                raise TypeError(f"alias {path} does not belong to this quick window")
            # remove the window path here, c++ code only requires the
            # object path from the root item.
            path = path[len(self.path) + 2:]

        data = self.client.send_command(
            'quick_item_find',
            quick_window_oid=self.oid,
            path=path,
            qid=id,
        )
        return Object(self.client, data)
