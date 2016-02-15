#include "ads/ContainerWidget.h"
#include "ads/Internal.h"
#include "ads/SectionTitleWidget.h"
#include "ads/SectionContentWidget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QContextMenuEvent>
#include <QMenu>
#include <QSplitter>
#include <QDataStream>
#include <QtGlobal>

ADS_NAMESPACE_BEGIN

// Static Helper //////////////////////////////////////////////////////

static QSplitter* newSplitter(Qt::Orientation orientation = Qt::Horizontal, QWidget* parent = 0)
{
	QSplitter* s = new QSplitter(orientation, parent);
	s->setChildrenCollapsible(false);
	s->setOpaqueResize(false);
	return s;
}

///////////////////////////////////////////////////////////////////////

ContainerWidget::ContainerWidget(QWidget *parent) :
	QFrame(parent),
	_mainLayout(NULL),
	_orientation(Qt::Horizontal),
	_splitter(NULL)
{
	_mainLayout = new QGridLayout();
	_mainLayout->setContentsMargins(9, 9, 9, 9);
	_mainLayout->setSpacing(0);
	setLayout(_mainLayout);
}

Qt::Orientation ContainerWidget::orientation() const
{
	return _orientation;
}

void ContainerWidget::setOrientation(Qt::Orientation orientation)
{
	if (_orientation != orientation)
	{
		_orientation = orientation;
		emit orientationChanged();
	}
}

SectionWidget* ContainerWidget::addSectionContent(const SectionContent::RefPtr& sc, SectionWidget* sw, DropArea area)
{
	if (!sw)
	{
		if (_sections.isEmpty())
		{	// Create default section
			sw = new SectionWidget(this);
			addSection(sw);
		}
		else if (area == CenterDropArea)
			// Use existing default section
			sw = _sections.first();
	}

	// Drop it based on "area"
	InternalContentData data;
	data.content = sc;
	data.titleWidget = new SectionTitleWidget(sc, NULL);
	data.contentWidget = new SectionContentWidget(sc, NULL);
	return dropContent(data, sw, area, false);
}

QMenu* ContainerWidget::createContextMenu() const
{
	QMenu* m = new QMenu(NULL);

	// Contents of SectionWidgets
	for (int i = 0; i < _sections.size(); ++i)
	{
		SectionWidget* sw = _sections.at(i);
		QList<SectionContent::RefPtr> contents = sw->contents();
		foreach (const SectionContent::RefPtr& c, contents)
		{
			QAction* a = m->addAction(QIcon(), c->uniqueName());
			a->setProperty("uid", c->uid());
			a->setProperty("type", "section");
			a->setCheckable(true);
			a->setChecked(c->titleWidget()->isVisible());
#if QT_VERSION >= 0x050000
			QObject::connect(a, &QAction::toggled, this, &ContainerWidget::onActionToggleSectionContentVisibility);
#else
			QObject::connect(a, SIGNAL(toggled(bool)), this, SLOT(onActionToggleSectionContentVisibility(bool)));
#endif
		}
	}

	// Contents of FloatingWidgets
	if (_floatingWidgets.size())
	{
		if (m->actions().size())
			m->addSeparator();
		for (int i = 0; i < _floatingWidgets.size(); ++i)
		{
			FloatingWidget* fw = _floatingWidgets.at(i);
			SectionContent::RefPtr c = fw->content();
			QAction* a = m->addAction(QIcon(), c->uniqueName());
			a->setProperty("uid", c->uid());
			a->setProperty("type", "floating");
			a->setCheckable(true);
			a->setChecked(fw->isVisible());
#if QT_VERSION >= 0x050000
			QObject::connect(a, &QAction::toggled, fw, &FloatingWidget::setVisible);
#else
			QObject::connect(a, SIGNAL(toggled(bool)), fw, SLOT(setVisible(bool)));
#endif
		}
	}

	return m;
}

QByteArray ContainerWidget::saveState() const
{
	QByteArray ba;
	QDataStream out(&ba, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_4_5);
	out << (quint32) 0x00001337; // Magic
	out << (quint32) 1; // Version

	// Save state of floating contents
	out << _floatingWidgets.count();
	for (int i = 0; i < _floatingWidgets.count(); ++i)
	{
		FloatingWidget* fw = _floatingWidgets.at(i);
		out << fw->content()->uniqueName();
		out << fw->saveGeometry();
	}

	// Walk through layout for splitters
	// Well.. there actually shouldn't be more than one
	for (int i = 0; i < _mainLayout->count(); ++i)
	{
		QLayoutItem* li = _mainLayout->itemAt(i);
		if (!li->widget())
			continue;
		saveGeometryWalk(out, li->widget());
	}

	return ba;
}

bool ContainerWidget::restoreState(const QByteArray& data)
{
	QDataStream in(data);
	in.setVersion(QDataStream::Qt_4_5);

	quint32 magic = 0;
	in >> magic;
	if (magic != 0x00001337)
		return false;

	quint32 version = 0;
	in >> version;
	if (version != 1)
		return false;

	QList<FloatingWidget*> oldFloatings = _floatingWidgets;
	QList<SectionWidget*> oldSections = _sections;

	// Restore floating widgets
	int fwCount = 0;
	in >> fwCount;
	if (fwCount > 0)
	{
		for (int i = 0; i < fwCount; ++i)
		{
			QString uname;
			in >> uname;
			QByteArray geom;
			in >> geom;

			SectionContent::RefPtr sc = SectionContent::LookupMapByName.value(uname).toStrongRef();
			if (!sc)
			{
				qWarning() << "Can not find floating widget section-content" << uname;
				continue;
			}
			InternalContentData data;
			if (!this->takeContent(sc, data))
				continue;

			FloatingWidget* fw = new FloatingWidget(this, sc, data.titleWidget, data.contentWidget, this);
			fw->restoreGeometry(geom);
		}
	}

	_sections.clear();

	// Restore splitters and section widgets
	const bool success = restoreGeometryWalk(in, NULL);
	if (success)
	{
		QLayoutItem* old = _mainLayout->takeAt(0);
		_mainLayout->addWidget(_splitter);
		delete old;
		qDeleteAll(oldFloatings);
		qDeleteAll(oldSections);
	}

	// TODO Handle contents which are not mentioned by deserialized data
	// ...

	return success;
}

///////////////////////////////////////////////////////////////////////
// PRIVATE API BEGINS HERE
///////////////////////////////////////////////////////////////////////

void ContainerWidget::splitSections(SectionWidget* s1, SectionWidget* s2, Qt::Orientation orientation)
{
	addSection(s1);

	if (!s2)
		s2 = new SectionWidget(this);
	addSection(s2);

	QSplitter* currentSplitter = findParentSplitter(s1);
	if (currentSplitter)
	{
		const int index = currentSplitter->indexOf(s1);
		QSplitter* splitter = newSplitter(orientation, this);
		splitter->addWidget(s1);
		splitter->addWidget(s2);
		currentSplitter->insertWidget(index, splitter);
	}
}

SectionWidget* ContainerWidget::dropContent(const InternalContentData& data, SectionWidget* targetSection, DropArea area, bool autoActive)
{
	SectionWidget* ret = NULL;

	// Drop on outer area
	if (!targetSection)
	{
		switch (area)
		{
		case TopDropArea:
			ret = dropContentOuterHelper(_mainLayout, data, Qt::Vertical, false);
			break;
		case RightDropArea:
			ret = dropContentOuterHelper(_mainLayout, data, Qt::Horizontal, true);
			break;
		case BottomDropArea:
			ret = dropContentOuterHelper(_mainLayout, data, Qt::Vertical, true);
			break;
		case LeftDropArea:
			ret = dropContentOuterHelper(_mainLayout, data, Qt::Horizontal, false);
			break;
		default:
			return NULL;
		}
		return NULL;
	}

	QSplitter* targetSectionSplitter = findParentSplitter(targetSection);

	// Drop logic based on area.
	switch (area)
	{
	case TopDropArea:
	{
		SectionWidget* sw = new SectionWidget(this);
		sw->addContent(data, true);
		if (targetSectionSplitter->orientation() == Qt::Vertical)
		{
			const int index = targetSectionSplitter->indexOf(targetSection);
			targetSectionSplitter->insertWidget(index, sw);
		}
		else
		{
			const int index = targetSectionSplitter->indexOf(targetSection);
			QSplitter* s = newSplitter(Qt::Vertical);
			s->addWidget(sw);
			s->addWidget(targetSection);
			targetSectionSplitter->insertWidget(index, s);
		}
		ret = sw;
		break;
	}
	case RightDropArea:
	{
		SectionWidget* sw = new SectionWidget(this);
		sw->addContent(data, true);
		if (targetSectionSplitter->orientation() == Qt::Horizontal)
		{
			const int index = targetSectionSplitter->indexOf(targetSection);
			targetSectionSplitter->insertWidget(index + 1, sw);
		}
		else
		{
			const int index = targetSectionSplitter->indexOf(targetSection);
			QSplitter* s = newSplitter(Qt::Horizontal);
			s->addWidget(targetSection);
			s->addWidget(sw);
			targetSectionSplitter->insertWidget(index, s);
		}
		ret = sw;
		break;
	}
	case BottomDropArea:
	{
		SectionWidget* sw = new SectionWidget(this);
		sw->addContent(data, true);
		if (targetSectionSplitter->orientation() == Qt::Vertical)
		{
			int index = targetSectionSplitter->indexOf(targetSection);
			targetSectionSplitter->insertWidget(index + 1, sw);
		}
		else
		{
			int index = targetSectionSplitter->indexOf(targetSection);
			QSplitter* s = newSplitter(Qt::Vertical);
			s->addWidget(targetSection);
			s->addWidget(sw);
			targetSectionSplitter->insertWidget(index, s);
		}
		ret = sw;
		break;
	}
	case LeftDropArea:
	{
		SectionWidget* sw = new SectionWidget(this);
		sw->addContent(data, true);
		if (targetSectionSplitter->orientation() == Qt::Horizontal)
		{
			int index = targetSectionSplitter->indexOf(targetSection);
			targetSectionSplitter->insertWidget(index, sw);
		}
		else
		{
			QSplitter* s = newSplitter(Qt::Horizontal);
			s->addWidget(sw);
			int index = targetSectionSplitter->indexOf(targetSection);
			targetSectionSplitter->insertWidget(index, s);
			s->addWidget(targetSection);
		}
		ret = sw;
		break;
	}
	case CenterDropArea:
	{
		targetSection->addContent(data, autoActive);
		ret = targetSection;
		break;
	}
	default:
		break;
	}
	return ret;
}

void ContainerWidget::addSection(SectionWidget* section)
{
	// Create default splitter.
	if (!_splitter)
	{
		_splitter = newSplitter(_orientation);
		_mainLayout->addWidget(_splitter, 0, 0);
	}
	if (_splitter->indexOf(section) != -1)
	{
		qWarning() << Q_FUNC_INFO << QString("Section has already been added");
		return;
	}
	_splitter->addWidget(section);
}

SectionWidget* ContainerWidget::sectionAt(const QPoint& pos) const
{
	const QPoint gpos = mapToGlobal(pos);
	for (int i = 0; i < _sections.size(); ++i)
	{
		SectionWidget* sw = _sections[i];
		if (sw->rect().contains(sw->mapFromGlobal(gpos)))
		{
			return sw;
		}
	}
	return 0;
}

QRect ContainerWidget::outerTopDropRect() const
{
	QRect r = rect();
	int h = r.height() / 100 * 5;
	return QRect(r.left(), r.top(), r.width(), h);
}

QRect ContainerWidget::outerRightDropRect() const
{
	QRect r = rect();
	int w = r.width() / 100 * 5;
	return QRect(r.right() - w, r.top(), w, r.height());
}

QRect ContainerWidget::outerBottomDropRect() const
{
	QRect r = rect();
	int h = r.height() / 100 * 5;
	return QRect(r.left(), r.bottom() - h, r.width(), h);
}

QRect ContainerWidget::outerLeftDropRect() const
{
	QRect r = rect();
	int w = r.width() / 100 * 5;
	return QRect(r.left(), r.top(), w, r.height());
}

SectionWidget* ContainerWidget::dropContentOuterHelper(QLayout* l, const InternalContentData& data, Qt::Orientation orientation, bool append)
{
	ContainerWidget* cw = this;
	SectionWidget* sw = new SectionWidget(cw);
	sw->addContent(data, true);

	QSplitter* oldsp = findImmediateSplitter(cw);
	if (oldsp->orientation() == orientation
			|| oldsp->count() == 1)
	{
		oldsp->setOrientation(orientation);
		if (append)
			oldsp->addWidget(sw);
		else
			oldsp->insertWidget(0, sw);
	}
	else
	{
		QSplitter* sp = newSplitter(orientation);
		if (append)
		{
#if QT_VERSION >= 0x050000
			QLayoutItem* li = l->replaceWidget(oldsp, sp);
			sp->addWidget(oldsp);
			sp->addWidget(sw);
			delete li;
#else
			int index = l->indexOf(oldsp);
			QLayoutItem* li = l->takeAt(index);
			sp->addWidget(oldsp);
			sp->addWidget(sw);
			delete li;
#endif
		}
		else
		{
#if QT_VERSION >= 0x050000
			sp->addWidget(sw);
			QLayoutItem* li = l->replaceWidget(oldsp, sp);
			sp->addWidget(oldsp);
			delete li;
#else
			sp->addWidget(sw);
			int index = l->indexOf(oldsp);
			QLayoutItem* li = l->takeAt(index);
			sp->addWidget(oldsp);
			delete li;
#endif
		}
	}
	return sw;
}

void ContainerWidget::saveGeometryWalk(QDataStream& out, QWidget* widget) const
{
	QSplitter* sp = NULL;
	SectionWidget* sw = NULL;

	if (!widget)
	{
		out << 0;
	}
	else if ((sp = dynamic_cast<QSplitter*>(widget)) != NULL)
	{
		out << 1; // Type = QSplitter
		out << ((sp->orientation() == Qt::Horizontal) ? (int) 1 : (int) 2);
		out << sp->count();
		out << sp->sizes();
		for (int i = 0; i < sp->count(); ++i)
		{
			saveGeometryWalk(out, sp->widget(i));
		}
	}
	else if ((sw = dynamic_cast<SectionWidget*>(widget)) != NULL)
	{
		out << 2; // Type = SectionWidget
		out << sw->currentIndex();
		out << sw->contents().count();
		const QList<SectionContent::RefPtr>& contents = sw->contents();
		for (int i = 0; i < contents.count(); ++i)
		{
			out << contents[i]->uniqueName();
		}
	}
}

bool ContainerWidget::restoreGeometryWalk(QDataStream& in, QSplitter* currentSplitter)
{
	int type;
	in >> type;

	// Splitter
	if (type == 1)
	{
		int orientation, count;
		QList<int> sizes;
		in >> orientation >> count >> sizes;

		QSplitter* sp = newSplitter((Qt::Orientation) orientation);
		for (int i = 0; i < count; ++i)
		{
			if (!restoreGeometryWalk(in, sp))
				return false;
		}
		sp->setSizes(sizes);

		if (!currentSplitter)
			_splitter = sp;
		else
			currentSplitter->addWidget(sp);
	}
	// Section
	else if (type == 2)
	{
		if (!currentSplitter)
		{
			qWarning() << "Missing splitter object for section";
			return false;
		}

		int currentIndex, count;
		in >> currentIndex >> count;

		SectionWidget* sw = new SectionWidget(this);
		for (int i = 0; i < count; ++i)
		{
			QString name;
			in >> name;
			SectionContent::RefPtr sc = SectionContent::LookupMapByName.value(name).toStrongRef();
			if (sc)
				sw->addContent(sc);
		}
		sw->setCurrentIndex(currentIndex);
		currentSplitter->addWidget(sw);
	}
	// Unknown
	else
	{
		qDebug() << QString();
	}

	return true;
}

bool ContainerWidget::takeContent(const SectionContent::RefPtr& sc, InternalContentData& data)
{
	// Search in sections
	bool found = false;
	for (int i = 0; i < _sections.count() && !found; ++i)
	{
		found = _sections.at(i)->takeContent(sc->uid(), data);
	}

	// Search in floating widgets
	for (int i = 0; i < _floatingWidgets.count() && !found; ++i)
	{
		found = _floatingWidgets.at(i)->content()->uid() == sc->uid();
		_floatingWidgets.at(i)->takeContent(data);
	}

	return found;
}

void ContainerWidget::onActionToggleSectionContentVisibility(bool visible)
{
	QAction* a = qobject_cast<QAction*>(sender());
	if (!a)
		return;
	const int uid = a->property("uid").toInt();
	qDebug() << "Change visibility of" << uid << visible;
}

ADS_NAMESPACE_END