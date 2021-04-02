/***************************************************************************
 *  Copyright (C) 2015 Alexander Golubev (Fat-Zer) <fatzer2@gmail.com>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
# include <QApplication>
# include <QMessageBox>
# include <QInputDialog>
# include <QCheckBox>
# include <Inventor/C/basic.h>
# include <TopExp_Explorer.hxx>
#endif

#include <boost/algorithm/string/predicate.hpp>

#include <Base/Console.h>
#include <App/Origin.h>
#include <App/Part.h>
#include <Gui/CommandT.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/Application.h>
#include <Gui/ActiveObjectList.h>
#include <Gui/MainWindow.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>

#include <Mod/Sketcher/App/SketchObject.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/Feature.h>
#include <Mod/PartDesign/App/FeatureBase.h>
#include <Mod/PartDesign/App/FeatureSketchBased.h>
#include <Mod/Part/Gui/PartParams.h>
#include <Mod/Part/App/PartParams.h>

#include "Utils.h"
#include "TaskFeaturePick.h"
#include "WorkflowManager.h"

FC_LOG_LEVEL_INIT("PartDesign", true, true);

//===========================================================================
// Shared functions
//===========================================================================

namespace PartDesignGui {

/// Returns active part, if there is no such, creates a new part, if it fails, shows a message
App::Part* assertActivePart () {
    App::Part* rv= Gui::Application::Instance->activeView()->getActiveObject<App::Part *> ( PARTKEY );

    if ( !rv ) {
        Gui::CommandManager &rcCmdMgr = Gui::Application::Instance->commandManager();
        rcCmdMgr.runCommandByName("Std_Part");
        rv = Gui::Application::Instance->activeView()->getActiveObject<App::Part *> ( PARTKEY );
        if ( !rv ) {
            QMessageBox::critical ( 0, QObject::tr( "Part creation failed" ),
                    QObject::tr( "Failed to create a part object." ) );
        }
    }

    return rv;
}

} /* PartDesignGui */

// PartDesign_Body
//===========================================================================
DEF_STD_CMD_A(CmdPartDesignBody)

CmdPartDesignBody::CmdPartDesignBody()
  : Command("PartDesign_Body")
{
    sAppModule    = "PartDesign";
    sGroup        = QT_TR_NOOP("PartDesign");
    sMenuText     = QT_TR_NOOP("Create body");
    sToolTipText  = QT_TR_NOOP("Create a new body and make it active");
    sWhatsThis    = "PartDesign_Body";
    sStatusTip    = sToolTipText;
    sPixmap       = "PartDesign_Body";
}

void CmdPartDesignBody::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    // if user decides for old-style workflow then abort the command
    if (PartDesignGui::assureLegacyWorkflow(getDocument()))
        return;

    auto activeDoc = App::GetApplication().getActiveDocument();
    if(!activeDoc)
        return;

    App::DocumentObject *topParent = nullptr;
    std::string topSubName;
    App::Part *actPart = PartDesignGui::getActivePart (&topParent, &topSubName);

    App::DocumentObject* baseFeature = nullptr;
    bool viewAll;
    bool addtogroup = false;

    openCommand(QT_TRANSLATE_NOOP("Command", "Add a Body"));

    App::DocumentObjectT labelSource;

    std::string support;
    std::string supportNames;
    auto sels = Gui::Selection().getSelectionT("*",0);
    int count = 0;
    for(auto it=sels.begin(); it!=sels.end();) {
        auto & sel = *it;
        auto sobj = sel.getSubObject();
        if (!sobj) {
            FC_WARN("failed to get sub object "
                    << sel.getDocumentName() << '#' << sel.getObjectName()
                    << '.' << sel.getSubName());
            it = sels.erase(it);
            continue;
        }
        if (sobj == actPart) {
            it = sels.erase(it);
            continue;
        }
        App::DocumentObject *owner = 0;
        auto shape = Part::Feature::getTopoShape(
                sel.getObject(), sel.getSubName().c_str(), true,0,&owner,false);
        if(!owner || !shape.hasSubShape(TopAbs_SOLID)) {
            FC_WARN("Ignore selection with no solid shape: "
                    << sel.getDocumentName() << '#' << sel.getObjectName()
                    << '.' << sel.getSubName());
            it = sels.erase(it);
            continue;
        }
        if (labelSource.getObjectName().empty())
            labelSource = sobj;
        ++count;
        ++it;
    }

    if(sels.empty()) {
        viewAll = true;
    } else {
        int count = 0;
        int numSolids = 0;
        int numShells = 0;
        std::ostringstream ss;
        std::ostringstream ss2;
        ss << '[';
        for(auto &sel : sels) {
            App::DocumentObject *owner = 0;
            App::DocumentObject *selObj = sel.getObject();
            auto shape = Part::Feature::getTopoShape(
                    selObj, sel.getSubName().c_str(), true,0,&owner,false);
            if(!owner || shape.isNull())
                continue;
            auto part = App::Part::getPartOfObject(owner);
            if(!count // first object
                    && owner->getDocument() == activeDoc // not an external object
                    && owner == selObj // not a sub-object
                    && owner == owner->getLinkedObject(true) // not a link
                    && !owner->isDerivedFrom(PartDesign::Body::getClassTypeId()) // not a body
                    && !PartDesign::Body::findBodyOf(owner) // not in a body
                    && (!part || part == actPart)) // not in a App::Part, or inside the active part
            {
                // Then we may use it directly as base feature
                baseFeature = owner;
            }

            auto link = selObj;
            std::string subname = sel.getSubName();
            if (topParent) {
                std::string s(topSubName);
                topParent->resolveRelativeLink(s, link, subname);
                if (!link)
                    continue;
            }
            ss << '(' << getObjectCmd(link) << ",'" << subname << "'), ";

            ++count;
            int cnt = shape.countSubShapes(TopAbs_SOLID);
            if(cnt)
                numSolids += cnt;
            else
                numShells += shape.countSubShapes(TopAbs_SHELL);

            if(link->getDocument() != activeDoc)
                ss2 << '\n' << link->getFullName();
            else
                ss2 << '\n' << link->getNameInDocument();
            if(subname.size())
                ss2 << '.' << subname;
            if(owner->Label.getStrValue() != owner->getNameInDocument())
                ss2 << " (" << owner->Label.getValue() << ")";
        }
        if(!count) {
            QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Bad base feature"),
                QObject::tr("No shape found in the selection."));
            return;
        }
        ss << "]";

        if(!baseFeature || count>1) {
            support = ss.str();
            baseFeature = 0;
        }
        supportNames = ss2.str();
    }

    if(supportNames.size()) {
        QMessageBox box(Gui::getMainWindow());
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QObject::tr("Base feature"));
        box. setText(QString::fromLatin1("%1\n%2\n\n%3").arg(
                    QObject::tr("You are about to use the following feature as base for the new body."),
                    QString::fromUtf8(supportNames.c_str()),
                    QObject::tr("Select 'Yes' to continue, 'No' to create the body without base "
                                "feature, or 'Abort' to exit.")));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Abort);
        box.setDefaultButton(QMessageBox::No);
        box.setEscapeButton(QMessageBox::Abort);

        QCheckBox checkBox(QObject::tr("Name body after the selection"));
        checkBox.setToolTip(QObject::tr("Name the new body after the selected object"));
        checkBox.setChecked(Part::PartParams::UseBaseObjectName());
        checkBox.blockSignals(true);
        box.addButton(&checkBox, QMessageBox::ResetRole);

        auto res = box.exec();
        if(res == QMessageBox::Abort)
            return;
        if (checkBox.isChecked() != Part::PartParams::UseBaseObjectName())
            Part::PartParams::set_UseBaseObjectName(checkBox.isChecked());
        if(res == QMessageBox::No)
            support.clear();
    } else
        labelSource = App::DocumentObjectT();

    std::string bodyName = getUniqueObjectName("Body", actPart);
    App::Document *doc = actPart ? actPart->getDocument() : App::GetApplication().getActiveDocument();

    // add the Body feature itself, and make it active
    Gui::cmdAppDocument(doc, std::ostringstream() << "addObject('PartDesign::Body','" << bodyName << "')");
    auto body = Base::freecad_dynamic_cast<PartDesign::Body>(doc->getObject(bodyName.c_str()));
    if (Part::PartParams::UseBaseObjectName() && labelSource.getObjectName().size())
        Gui::cmdAppObjectArgs(body, "Label = %s.Label", labelSource.getObjectPython());
    if (baseFeature) {
        if (baseFeature->isDerivedFrom(Part::Part2DObject::getClassTypeId())) {
            Gui::cmdAppObjectArgs(body, "Group = [%s]", getObjectCmd(baseFeature));
        }
        else {
            Gui::cmdAppObjectArgs(body, "BaseFeature = %s", getObjectCmd(baseFeature));
        }
    }

    if (actPart) {
        Gui::cmdAppObjectArgs(actPart, "addObject(%s)", getObjectCmd(body));
    }

    addModule(Gui,"PartDesignGui"); // import the Gui module only once a session

    // Make the "Create sketch" prompt appear in the task panel
    doCommand(Gui,"Gui.Selection.clearSelection()");
    if (topParent) {
        doCommand(Gui,"Gui.Selection.addSelection(%s, '%s')",
                getObjectCmd(topParent).c_str(), (topSubName + bodyName + ".").c_str());
    } else
        doCommand(Gui,"Gui.Selection.addSelection(%s)", getObjectCmd(body).c_str());

    doCommand(Gui::Command::Gui, "Gui.activateView('Gui::View3DInventor', True)\n"
                                 "Gui.activeView().setActiveObject('%s', %s)",
            PDBODYKEY, getObjectCmd(body).c_str());

    // check if a proxy object has been created for the base feature inside the body
    if (baseFeature) {
        if (body) {
            std::vector<App::DocumentObject*> links = body->Group.getValues();
            for (auto it : links) {
                if (it->getTypeId().isDerivedFrom(PartDesign::FeatureBase::getClassTypeId())) {
                    PartDesign::FeatureBase* base = static_cast<PartDesign::FeatureBase*>(it);
                    if (base && base->BaseFeature.getValue() == baseFeature) {
                        Gui::Application::Instance->hideViewProvider(baseFeature);
                        break;
                    }
                }
            }

            // for sketches open the feature dialog to rebase it to a new plane
            // as requested in issue #0002862
            if (addtogroup) {
                std::vector<App::DocumentObject*> planes;
                std::vector<PartDesignGui::TaskFeaturePick::featureStatus> status;
                unsigned validPlaneCount = 0;
                for (auto plane: body->getOrigin ()->planes()) {
                    planes.push_back (plane);
                    status.push_back(PartDesignGui::TaskFeaturePick::basePlane);
                    validPlaneCount++;
                }

                if (validPlaneCount > 1) {
                    // Determines if user made a valid selection in dialog
                    auto accepter = [](const std::vector<App::DocumentObject*>& features) -> bool {
                        return !features.empty();
                    };

                    // Called by dialog when user hits "OK" and accepter returns true
                    auto worker = [baseFeature](const std::vector<App::DocumentObject*>& features) {
                        // may happen when the user switched to an empty document while the
                        // dialog is open
                        if (features.empty())
                            return;
                        App::Plane* plane = static_cast<App::Plane*>(features.front());
                        std::string supportString = Gui::Command::getObjectCmd(plane,"(",", [''])");

                        FCMD_OBJ_CMD(baseFeature,"Support = " << supportString);
                        FCMD_OBJ_CMD(baseFeature,"MapMode = '" << Attacher::AttachEngine::getModeName(Attacher::mmFlatFace) << "'");
                        Gui::Command::updateActive();
                    };

                    // Called by dialog for "Cancel", or "OK" if accepter returns false
                    std::string docname = getDocument()->getName();
                    auto quitter = [docname]() {
                        Gui::Document* document = Gui::Application::Instance->getDocument(docname.c_str());
                        if (document)
                            document->abortCommand();
                    };

                    // Show dialog and let user pick plane
                    Gui::TaskView::TaskDialog *dlg = Gui::Control().activeDialog();
                    if (!dlg) {
                        Gui::Selection().clearSelection();
                        Gui::Control().showDialog(new PartDesignGui::TaskDlgFeaturePick(planes, status, accepter, worker, true, quitter));
                    }
                }
            }
        }
    } else if (body && support.size()) {
        std::string name = getUniqueObjectName("BaseFeature", body);
        Gui::cmdAppDocument(doc, std::ostringstream() << "addObject('PartDesign::SubShapeBinder','" << name << "')");

        baseFeature = doc->getObject(name.c_str());
        if(baseFeature) {
            FCMD_OBJ_CMD(body,"addObject(" << getObjectCmd(baseFeature) << ")");
            FCMD_OBJ_CMD(body,"BaseFeature = " << getObjectCmd(baseFeature));
            if (!boost::starts_with(baseFeature->Label.getValue(), "BaseFeature"))
                FCMD_OBJ_CMD(baseFeature,"Label = " << getObjectCmd(baseFeature) << ".Label");
            FCMD_OBJ_CMD(baseFeature,"Fuse = True");
            FCMD_OBJ_CMD(baseFeature,"Support = " << support);
        }
    }

    // if no part feature was there then auto-adjust the camera
    if (viewAll && PartGui::PartParams::AdjustCameraForNewFeature()) {
        Gui::Document* doc = Gui::Application::Instance->getDocument(getDocument());
        Gui::View3DInventor* view = doc ? qobject_cast<Gui::View3DInventor*>(doc->getActiveView()) : nullptr;
        if (view) {
            view->getViewer()->viewSelection(true);
        }
    }

    updateActive();

    if (triggerSource() == TriggerAction)
        Gui::setTreeViewFocus();
}

bool CmdPartDesignBody::isActive(void)
{
    return hasActiveDocument() && !PartDesignGui::isLegacyWorkflow ( getDocument () );
}

//===========================================================================
// PartDesign_Migrate
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignMigrate)

CmdPartDesignMigrate::CmdPartDesignMigrate()
  : Command("PartDesign_Migrate")
{
    sAppModule    = "PartDesign";
    sGroup        = QT_TR_NOOP("PartDesign");
    sMenuText     = QT_TR_NOOP("Migrate");
    sToolTipText  = QT_TR_NOOP("Migrate document to the modern PartDesign workflow");
    sWhatsThis    = "PartDesign_Migrate";
    sStatusTip    = sToolTipText;
    sPixmap       = "PartDesign_Migrate";
}

void CmdPartDesignMigrate::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    App::Document *doc = getDocument();

    std::set<PartDesign::Feature*> migrateFeatures;


    // Retrieve all PartDesign Features objects and filter out features already belonging to some body
    for ( const auto & feat: doc->getObjects(  ) ) {
         if( feat->isDerivedFrom( PartDesign::Feature::getClassTypeId() ) &&
                 !PartDesign::Body::findBodyOf( feat ) && PartDesign::Body::isSolidFeature ( feat ) ) {
             migrateFeatures.insert ( static_cast <PartDesign::Feature *>( feat ) );
         }
    }

    if ( migrateFeatures.empty() ) {
        if ( !PartDesignGui::isModernWorkflow ( doc ) ) {
            // If there is nothing to migrate and workflow is still old just set it to modern
            PartDesignGui::WorkflowManager::instance()->forceWorkflow (
                    doc, PartDesignGui::Workflow::Modern );
        } else {
            // Huh? nothing to migrate?
            QMessageBox::warning ( 0, QObject::tr ( "Nothing to migrate" ),
                    QObject::tr ( "No PartDesign features found that don't belong to a body."
                        " Nothing to migrate." ) );
        }
        return;
    }

    // Note: this action is undoable, should it be?
    PartDesignGui::WorkflowManager::instance()->forceWorkflow ( doc, PartDesignGui::Workflow::Modern );

    // Put features into chains. Each chain should become a separate body.
    std::list< std::list<PartDesign::Feature *> > featureChains;
    std::list<PartDesign::Feature *> chain; //< the current chain we are working on

    for (auto featIt = migrateFeatures.begin(); !migrateFeatures.empty(); ) {
        Part::Feature *base = (*featIt)->getBaseObject( /*silent =*/ true );

        chain.push_front ( *featIt );

        if ( !base || !base->isDerivedFrom (PartDesign::Feature::getClassTypeId () ) ||
                PartDesignGui::isAnyNonPartDesignLinksTo ( static_cast <PartDesign::Feature *>(base),
                                                           /*respectGroups=*/ true ) ) {
            // a feature based on nothing as well as on non-partdesign solid starts a new chain
            auto newChainIt = featureChains.emplace (featureChains.end());
            newChainIt->splice (newChainIt->end(), chain);
        } else {
            // we are basing on some partdesign feature which supposed to belong to some body
            PartDesign::Feature *baseFeat = static_cast <PartDesign::Feature *>( base );

            auto baseFeatSetIt = migrateFeatures.find(baseFeat);

            if ( baseFeatSetIt != migrateFeatures.end() ) {
                // base feature is pending for migration, switch to it and continue over
                migrateFeatures.erase(featIt);
                featIt = baseFeatSetIt;
                continue;
            } else {
                // The base feature seems already assigned to some chain
                // Find which
                std::list<PartDesign::Feature *>::iterator baseFeatIt;
                auto chainIt = std::find_if( featureChains.begin(), featureChains.end(),
                        [baseFeat, &baseFeatIt] ( std::list<PartDesign::Feature *>&chain ) mutable -> bool {
                            baseFeatIt = std::find( chain.begin(), chain.end(), baseFeat );
                            return baseFeatIt !=  chain.end();
                        } );

                if ( chainIt != featureChains.end() ) {
                    assert (baseFeatIt != chainIt->end());
                    if ( std::next ( baseFeatIt ) == chainIt->end() ) {
                        // just append our chain to already found
                        chainIt->splice ( chainIt->end(), chain );
                        // TODO: If we will hit a third part everything will be messed up again.
                        //       Probably it will require a yet another smart-ass find_if. (2015-08-10, Fat-Zer)
                    } else {
                        // We have a fork of a partDesign feature here
                        // add a chain for current body
                        auto newChainIt = featureChains.emplace (featureChains.end());
                        newChainIt->splice (newChainIt->end(), chain);
                        // add a chain for forked one
                        newChainIt = featureChains.emplace (featureChains.end());
                        newChainIt->splice (newChainIt->end(), *chainIt,
                                std::next ( baseFeatIt ), chainIt->end());
                    }
                } else {
                    // The feature is not present in list pending for migration,
                    // This generally shouldn't happen but may be if we run into some broken file
                    // Try to find out the body we should insert into
                    // TODO: Some error/warning is needed here (2015-08-10, Fat-Zer)
                    auto newChainIt = featureChains.emplace (featureChains.end());
                    newChainIt->splice (newChainIt->end(), chain);
                }
            }
        }
        migrateFeatures.erase ( featIt );
        featIt = migrateFeatures.begin ();
        // TODO: Align visibility (2015-08-17, Fat-Zer)
    } /* for */

    // TODO: make it work without parts (2015-09-04, Fat-Zer)
    // add a part if there is no active yet
    App::Part *actPart = PartDesignGui::assertActivePart ();

    if (!actPart) {
        return;
    }

    // do the actual migration
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Migrate legacy part design features to Bodies"));

    for ( auto chainIt = featureChains.begin(); !featureChains.empty();
            featureChains.erase (chainIt), chainIt = featureChains.begin () ) {
#ifndef FC_DEBUG
        if ( chainIt->empty () ) { // prevent crash in release in case of errors
            continue;
        }
#else
        assert ( !chainIt->empty () );
#endif
        Part::Feature *base = chainIt->front()->getBaseObject ( /*silent =*/ true );

        // Find a suitable chain to work with
        for( ; chainIt != featureChains.end(); chainIt ++) {
            base = chainIt->front()->getBaseObject ( /*silent =*/ true );
            if (!base || !base->isDerivedFrom ( PartDesign::Feature::getClassTypeId () ) ) {
                break;   // no base is ok
            } else {
                // The base feature is a PartDesign, it's a fork, try to reassign it to a body...
                base = PartDesign::Body::findBodyOf ( base );
                if ( base ) {
                    break;
                }
            }
        }

        if ( chainIt == featureChains.end() ) {
            // Shouldn't happen, may be only in case of some circular dependency?
            // TODO Some error message (2015-08-11, Fat-Zer)
            chainIt = featureChains.begin();
            base = chainIt->front()->getBaseObject ( /*silent =*/ true );
        }

        // Construct a Pretty Body name based on the Tip
        std::string bodyName = getUniqueObjectName (
                std::string ( chainIt->back()->getNameInDocument() ).append ( "Body" ).c_str () ) ;

        // Create a body for the chain
        doCommand ( Doc,"App.activeDocument().addObject('PartDesign::Body','%s')", bodyName.c_str () );
        doCommand ( Doc,"App.activeDocument().%s.addObject(App.ActiveDocument.%s)",
                actPart->getNameInDocument (), bodyName.c_str () );
        if (base) {
            doCommand ( Doc,"App.activeDocument().%s.BaseFeature = App.activeDocument().%s",
                bodyName.c_str (), base->getNameInDocument () );
        }

        // Fill the body with features
        for ( auto feature: *chainIt ) {
            if ( feature->isDerivedFrom ( PartDesign::ProfileBased::getClassTypeId() ) ) {
                // add the sketch and also reroute it if needed
                PartDesign::ProfileBased *sketchBased = static_cast<PartDesign::ProfileBased *> ( feature );
                Part::Part2DObject *sketch = sketchBased->getVerifiedSketch( /*silent =*/ true);
                if ( sketch ) {
                    doCommand ( Doc,"App.activeDocument().%s.addObject(App.activeDocument().%s)",
                            bodyName.c_str (), sketch->getNameInDocument() );

                    if ( sketch->isDerivedFrom ( Sketcher::SketchObject::getClassTypeId() ) ) {
                        try {
                            PartDesignGui::fixSketchSupport ( static_cast<Sketcher::SketchObject *> ( sketch ) );
                        } catch (Base::Exception &) {
                            QMessageBox::critical(Gui::getMainWindow(),
                                    QObject::tr("Sketch plane cannot be migrated"),
                                    QObject::tr("Please edit '%1' and redefine it to use a Base or "
                                        "Datum plane as the sketch plane.").
                                    arg(QString::fromUtf8(sketch->Label.getValue()) ) );
                        }
                    } else {
                        // TODO: Message that sketchbased is based not on a sketch (2015-08-11, Fat-Zer)
                    }
                }
            }
            doCommand ( Doc,"App.activeDocument().%s.addObject(App.activeDocument().%s)",
                    bodyName.c_str (), feature->getNameInDocument() );

            PartDesignGui::relinkToBody ( feature );
        }

    }

    updateActive();
}

bool CmdPartDesignMigrate::isActive(void)
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_MoveTip
//===========================================================================
DEF_STD_CMD_A(CmdPartDesignMoveTip)

CmdPartDesignMoveTip::CmdPartDesignMoveTip()
  : Command("PartDesign_MoveTip")
{
    sAppModule    = "PartDesign";
    sGroup        = QT_TR_NOOP("PartDesign");
    sMenuText     = QT_TR_NOOP("Set tip");
    sToolTipText  = QT_TR_NOOP("Move the tip of the body");
    sWhatsThis    = "PartDesign_MoveTip";
    sStatusTip    = sToolTipText;
    sPixmap       = "PartDesign_MoveTip";
}

void CmdPartDesignMoveTip::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    std::vector<App::DocumentObject*> features = getSelection().getObjectsOfType(
            Part::Feature::getClassTypeId() );
    App::DocumentObject* selFeature;
    PartDesign::Body* body= nullptr;

    if ( features.size() == 1 ) {
        selFeature = features.front();
        if ( selFeature->getTypeId().isDerivedFrom ( PartDesign::Body::getClassTypeId() ) ) {
            body = static_cast<PartDesign::Body *> ( selFeature );
        } else {
            body = PartDesignGui::getBodyFor ( selFeature, /* messageIfNot =*/ false );
        }
    } else {
        selFeature = nullptr;
    }

    if (!selFeature) {
        QMessageBox::warning (0, QObject::tr( "Selection error" ),
                QObject::tr( "Select exactly one PartDesign feature or a body." ) );
        return;
    } else if (!body) {
        QMessageBox::warning (0, QObject::tr( "Selection error" ),
                QObject::tr( "Couldn't determine a body for the selected feature '%s'.", selFeature->Label.getValue() ) );
        return;
    } else if ( !selFeature->isDerivedFrom(PartDesign::Feature::getClassTypeId () ) &&
            selFeature != body && body->BaseFeature.getValue() != selFeature ) {
        QMessageBox::warning (0, QObject::tr( "Selection error" ),
                QObject::tr( "Only a solid feature can be the tip of a body." ) );
        return;
    }

    App::DocumentObject* oldTip = body->Tip.getValue();
    if (oldTip == selFeature) { // it's not generally an error, so print only a console message
        Base::Console().Message ("%s is already the tip of the body\n", selFeature->getNameInDocument () );
        return;
    }

    openCommand(QT_TRANSLATE_NOOP("Command", "Move tip to selected feature"));

    if (selFeature == body) {
        FCMD_OBJ_CMD(body,"Tip = None");
    } else {
        FCMD_OBJ_CMD(body,"Tip = " << getObjectCmd(selFeature));

        // Adjust visibility to show only the Tip feature
        FCMD_OBJ_SHOW(selFeature);
    }

    // TODO: Hide all datum features after the Tip feature? But the user might have already hidden some and wants to see
    // others, so we would have to remember their state somehow
    updateActive();
}

bool CmdPartDesignMoveTip::isActive(void)
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_DuplicateSelection
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignDuplicateSelection)

CmdPartDesignDuplicateSelection::CmdPartDesignDuplicateSelection()
  :Command("PartDesign_DuplicateSelection")
{
    sAppModule      = "PartDesign";
    sGroup          = QT_TR_NOOP("PartDesign");
    sMenuText       = QT_TR_NOOP("Duplicate selected object");
    sToolTipText    = QT_TR_NOOP("Duplicates the selected object and adds it to the active body");
    sWhatsThis      = "PartDesign_DuplicateSelection";
    sStatusTip      = sToolTipText;
}

void CmdPartDesignDuplicateSelection::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body *pcActiveBody = PartDesignGui::getBody(/*messageIfNot = */false);

    std::vector<App::DocumentObject*> beforeFeatures = getDocument()->getObjects();

    openCommand(QT_TRANSLATE_NOOP("Command", "Duplicate a PartDesign object"));
    doCommand(Doc,"FreeCADGui.runCommand('Std_DuplicateSelection')");

    if (pcActiveBody) {
        // Find the features that were added
        std::vector<App::DocumentObject*> afterFeatures = getDocument()->getObjects();
        std::vector<App::DocumentObject*> newFeatures;
        std::sort(beforeFeatures.begin(), beforeFeatures.end());
        std::sort(afterFeatures.begin(), afterFeatures.end());
        std::set_difference(afterFeatures.begin(), afterFeatures.end(), beforeFeatures.begin(), beforeFeatures.end(),
                            std::back_inserter(newFeatures));

        for (auto feature : newFeatures) {
            if (PartDesign::Body::isAllowed(feature)) {
                FCMD_OBJ_CMD(pcActiveBody,"addObject(" << getObjectCmd(feature) << ")");
                FCMD_OBJ_HIDE(feature);
            }
        }

        // Adjust visibility of features
        if (!newFeatures.empty())
            FCMD_OBJ_SHOW(newFeatures.back());
    }

    updateActive();
}

bool CmdPartDesignDuplicateSelection::isActive(void)
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_MoveFeature
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignMoveFeature)

CmdPartDesignMoveFeature::CmdPartDesignMoveFeature()
  :Command("PartDesign_MoveFeature")
{
    sAppModule      = "PartDesign";
    sGroup          = QT_TR_NOOP("PartDesign");
    sMenuText       = QT_TR_NOOP("Move object to other body");
    sToolTipText    = QT_TR_NOOP("Moves the selected object to another body");
    sWhatsThis      = "PartDesign_MoveFeature";
    sStatusTip      = sToolTipText;
    sPixmap         = "PartDesign_MoveFeature";
}

void CmdPartDesignMoveFeature::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    std::vector<App::DocumentObject*> features = getSelection().getObjectsOfType(Part::Feature::getClassTypeId());
    if (features.empty()) return;

    // Check if all features are valid to move
    if (std::any_of(std::begin(features), std::end(features), [](App::DocumentObject* obj){return !PartDesignGui::isFeatureMovable(obj); }))
    {
        //show messagebox and cancel
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Features cannot be moved"),
            QObject::tr("Some of the selected features have dependencies in the source body"));
        return;
    }

    // Collect dependencies of the selected features
    std::vector<App::DocumentObject*> dependencies = PartDesignGui::collectMovableDependencies(features);
    if (!dependencies.empty())
        features.insert(std::end(features), std::begin(dependencies), std::end(dependencies));

    // Create a list of all bodies in this part
    std::vector<App::DocumentObject*> bodies = getDocument()->getObjectsOfType(Part::BodyBase::getClassTypeId());

    std::set<App::DocumentObject*> source_bodies;
    for (auto feat : features) {
        // Note: 'source' can be null which means that the feature doesn't belong to a body.
        PartDesign::Body* source = PartDesign::Body::findBodyOf(feat);
        source_bodies.insert(static_cast<App::DocumentObject*>(source));
    }

    if (source_bodies.size() != 1) {
        //show messagebox and cancel
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Features cannot be moved"),
            QObject::tr("Only features of a single source Body can be moved"));
        return;
    }

    auto source_body = *source_bodies.begin();

    std::vector<App::DocumentObject*> target_bodies;
    for (auto body : bodies) {
        if (!source_bodies.count(body))
            target_bodies.push_back(body);
    }

    if (target_bodies.empty())
    {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Features cannot be moved"),
            QObject::tr("There are no other bodies to move to"));
        return;
    }

    // Ask user to select the target body (remove source bodies from list)
    bool ok;
    QStringList items;
    for (auto body : target_bodies) {
        items.push_back(QString::fromUtf8(body->Label.getValue()));
    }
    QString text = QInputDialog::getItem(Gui::getMainWindow(),
        qApp->translate("PartDesign_MoveFeature", "Select body"),
        qApp->translate("PartDesign_MoveFeature", "Select a body from the list"),
        items, 0, false, &ok, Qt::MSWindowsFixedSizeDialogHint);
    if (!ok) return;
    int index = items.indexOf(text);
    if (index < 0) return;

    PartDesign::Body* target = static_cast<PartDesign::Body*>(target_bodies[index]);

    openCommand(QT_TRANSLATE_NOOP("Command", "Move an object"));

    std::stringstream stream;
    stream << "features_ = [" << getObjectCmd(features.back());
    features.pop_back();

    for (auto feat: features)
        stream << ", " << getObjectCmd(feat);

    stream << "]";
    runCommand(Doc, stream.str().c_str());
    FCMD_OBJ_CMD(source_body,"removeObjects(features_)");
    FCMD_OBJ_CMD(target,"addObjects(features_)");
    /*

        // Find body of this feature
        Part::BodyBase* source = PartDesign::Body::findBodyOf(feat);
        bool featureWasTip = false;

        if (source == target) continue;

        // Remove from the source body if the feature belonged to a body
        if (source) {
            featureWasTip = (source->Tip.getValue() == feat);
            doCommand(Doc,"App.activeDocument().%s.removeObject(App.activeDocument().%s)",
                      source->getNameInDocument(), (feat)->getNameInDocument());
        }

        App::DocumentObject* targetOldTip = target->Tip.getValue();

        // Add to target body (always at the Tip)
        doCommand(Doc,"App.activeDocument().%s.addObject(App.activeDocument().%s)",
                      target->getNameInDocument(), (feat)->getNameInDocument());
        // Recompute to update the shape
        doCommand(Gui,"App.activeDocument().recompute()");

        // Adjust visibility of features
        // TODO: May be something can be done in view provider (2015-08-05, Fat-Zer)
        // If we removed the tip of the source body, make the new tip visible
        if ( featureWasTip ) {
            App::DocumentObject * sourceNewTip = source->Tip.getValue();
            if (sourceNewTip)
                doCommand(Gui,"Gui.activeDocument().show(\"%s\")", sourceNewTip->getNameInDocument());
        }

        // Hide old tip and show new tip (the moved feature) of the target body
        App::DocumentObject* targetNewTip = target->Tip.getValue();
        if ( targetOldTip != targetNewTip ) {
            if ( targetOldTip ) {
                doCommand(Gui,"Gui.activeDocument().hide(\"%s\")", targetOldTip->getNameInDocument());
            }
            if (targetNewTip) {
                doCommand(Gui,"Gui.activeDocument().show(\"%s\")", targetNewTip->getNameInDocument());
            }
        }

        // Fix sketch support
        if (feat->getTypeId().isDerivedFrom(Sketcher::SketchObject::getClassTypeId())) {
            Sketcher::SketchObject *sketch = static_cast<Sketcher::SketchObject*>(feat);
            try {
                PartDesignGui::fixSketchSupport(sketch);
            } catch (Base::Exception &) {
                QMessageBox::warning( Gui::getMainWindow(), QObject::tr("Sketch plane cannot be migrated"),
                        QObject::tr("Please edit '%1' and redefine it to use a Base or Datum plane as the sketch plane.").
                        arg( QString::fromLatin1( sketch->Label.getValue () ) ) );
            }
        }

        //relink origin for sketches and datums (coordinates)
        PartDesignGui::relinkToOrigin(feat, target);
    }*/

    updateActive();
}

bool CmdPartDesignMoveFeature::isActive(void)
{
    return hasActiveDocument () && !PartDesignGui::isLegacyWorkflow ( getDocument () );
}

DEF_STD_CMD_A(CmdPartDesignMoveFeatureInTree)

CmdPartDesignMoveFeatureInTree::CmdPartDesignMoveFeatureInTree()
  :Command("PartDesign_MoveFeatureInTree")
{
    sAppModule      = "PartDesign";
    sGroup          = QT_TR_NOOP("PartDesign");
    sMenuText       = QT_TR_NOOP("Move object after other object");
    sToolTipText    = QT_TR_NOOP("Moves the selected object and insert it after another object");
    sWhatsThis      = "PartDesign_MoveFeatureInTree";
    sStatusTip      = sToolTipText;
    sPixmap         = "PartDesign_MoveFeatureInTree";
}

void CmdPartDesignMoveFeatureInTree::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    std::vector<App::DocumentObject*> features = getSelection().getObjectsOfType(Part::Feature::getClassTypeId());
    if (features.empty()) return;

    PartDesign::Body *body = PartDesignGui::getBodyFor ( features.front(), false );
    App::DocumentObject * bodyBase = nullptr;
    // sanity check
    bool allFeaturesFromSameBody = true;

    if ( body ) {
        bodyBase = body->BaseFeature.getValue();
        for ( auto feat: features ) {
            if ( !body->hasObject ( feat ) ) {
                allFeaturesFromSameBody = false;
                break;
            }
            if ( bodyBase == feat) {
                QMessageBox::warning (0, QObject::tr( "Selection error" ),
                        QObject::tr( "Impossible to move the base feature of a body." ) );
                return;
            }
        }
    }
    if (!body || ! allFeaturesFromSameBody) {
        QMessageBox::warning (0, QObject::tr( "Selection error" ),
                QObject::tr( "Select one or more features from the same body." ) );
        return;
    }

    // Create a list of all features in this body
    const std::vector<App::DocumentObject*> & model = body->Group.getValues();

    // Ask user to select the target feature
    bool ok;
    QStringList items;
    if ( bodyBase ) {
        items.push_back( QString::fromUtf8 ( bodyBase->Label.getValue () ) );
    } else {
        items.push_back( QObject::tr( "Beginning of the body" ) );
    }
    for ( auto feat: model ) {
        items.push_back( QString::fromUtf8 ( feat->Label.getValue() ) );
    }

    QString text = QInputDialog::getItem(Gui::getMainWindow(),
        qApp->translate("PartDesign_MoveFeatureInTree", "Select feature"),
        qApp->translate("PartDesign_MoveFeatureInTree", "Select a feature from the list"),
        items, 0, false, &ok, Qt::MSWindowsFixedSizeDialogHint);
    if (!ok) return;
    int index = items.indexOf(text);
    // first object is the beginning of the body
    App::DocumentObject* target = index != 0 ? model[index-1] : nullptr;

    openCommand(QT_TRANSLATE_NOOP("Command", "Move an object inside tree"));

    App::DocumentObject* lastObject = nullptr;
    for ( auto feat: features ) {
        if ( feat == target ) continue;

        // Remove and re-insert the feature to/from the Body
        // TODO: if tip was moved the new position of tip is quite undetermined (2015-08-07, Fat-Zer)
        // TODO: warn the user if we are moving an object to some place before the object's link (2015-08-07, Fat-Zer)
        FCMD_OBJ_CMD(body,"removeObject(" << getObjectCmd(feat) << ")");
        FCMD_OBJ_CMD(body,"insertObject(" << getObjectCmd(feat) << ","<< getObjectCmd(target) << ", True)");

        if (!lastObject)
            lastObject = feat;
    }

    // Dependency order check.
    // We must make sure the resulting objects of PartDesign::Feature do not
    // depend on later objects
    std::vector<App::DocumentObject*> bodyFeatures;
    std::map<App::DocumentObject*,size_t> orders;
    for(auto obj : body->Group.getValues()) {
        if(obj->isDerivedFrom(PartDesign::Feature::getClassTypeId())) {
            orders.emplace(obj,bodyFeatures.size());
            bodyFeatures.push_back(obj);
        }
    }
    bool failed = false;
    std::ostringstream ss;
    for(size_t i=0;i<bodyFeatures.size();++i) {
        auto feat = bodyFeatures[i];
        for(auto obj : feat->getOutList()) {
            if(obj->isDerivedFrom(PartDesign::Feature::getClassTypeId()))
                continue;
            for(auto dep : App::Document::getDependencyList({obj})) {
                auto it = orders.find(dep);
                if(it != orders.end() && it->second > i) {
                    ss << feat->Label.getValue() << ", " <<
                        obj->Label.getValue() << " -> " <<
                        it->first->Label.getValue();
                    if(!failed)
                        failed = true;
                    else
                        ss << std::endl;
                }
            }
        }
    }
    if(failed) {
        QMessageBox::critical (0, QObject::tr( "Dependency violation" ),
                QObject::tr( "Early feature must not depend on later feature.\n\n")
                    + QString::fromUtf8(ss.str().c_str()));
        abortCommand();
        return;
    }

    // If the selected objects have been moved after the current tip then ask the
    // user if he wants the last object to be the new tip.
    // Only do this for features that can hold a tip (not for e.g. datums)
    if ( lastObject && body->Tip.getValue() == target
        && lastObject->isDerivedFrom(PartDesign::Feature::getClassTypeId()) ) {
        QMessageBox msgBox(Gui::getMainWindow());
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setWindowTitle(qApp->translate("PartDesign_MoveFeatureInTree", "Move tip"));
        msgBox.setText(qApp->translate("PartDesign_MoveFeatureInTree", "The moved feature appears after the currently set tip."));
        msgBox.setInformativeText(qApp->translate("PartDesign_MoveFeatureInTree", "Do you want the last feature to be the new tip?"));
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::No);
        int ret = msgBox.exec();
        if (ret == QMessageBox::Yes)
            FCMD_OBJ_CMD(body, "Tip = " << getObjectCmd(lastObject));
    }

    updateActive();
}

bool CmdPartDesignMoveFeatureInTree::isActive(void)
{
    return hasActiveDocument () && !PartDesignGui::isLegacyWorkflow ( getDocument () );
}


//===========================================================================
// Initialization
//===========================================================================

void CreatePartDesignBodyCommands(void)
{
    Gui::CommandManager &rcCmdMgr = Gui::Application::Instance->commandManager();

    rcCmdMgr.addCommand(new CmdPartDesignBody());
    rcCmdMgr.addCommand(new CmdPartDesignMigrate());
    rcCmdMgr.addCommand(new CmdPartDesignMoveTip());

    rcCmdMgr.addCommand(new CmdPartDesignDuplicateSelection());
    rcCmdMgr.addCommand(new CmdPartDesignMoveFeature());
    rcCmdMgr.addCommand(new CmdPartDesignMoveFeatureInTree());
}