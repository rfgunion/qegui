/*  QEGui.cpp
 *
 *  This file is part of the EPICS QT Framework, initially developed at the Australian Synchrotron.
 *
 *  The EPICS QT Framework is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  The EPICS QT Framework is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with the EPICS QT Framework.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Copyright (c) 2009, 2010 Australian Synchrotron
 *
 *  Author:
 *    Andrew Rhyder
 *  Contact details:
 *    andrew.rhyder@synchrotron.org.au
 */

/*
 This class is used to manage the overall QEGui application.
 Note, each main window is managed by the QEMainWindow class. This class manages anything not common to all main windows.
 */

#include "QEGui.h"
#include <iostream>
#include <QEFrameworkVersion.h>
#include <InstanceManager.h>
#include <QDebug>
#include <saveRestoreManager.h>
#include <QSettings>
#include <QEScaling.h>
#include <QEForm.h>
#include <QMetaType>
#include <QVariant>
#include <QMetaType>
#include <QMessageBox>
#include <QDateTime>
#include <signal.h>

Q_DECLARE_METATYPE( QEForm* )


static void unixSignalHandler(int signum) {

    Q_UNUSED(signum);

    //qDebug("DBG: main.cpp::unixSignalHandler(). signal = %s\n", strsignal(signum));

    /*
     * Make sure your Qt application gracefully quits.
     * NOTE - purpose for calling qApp->exit(0):
     *      1. Forces the Qt framework's "main event loop `qApp->exec()`" to quit looping.
     *      2. Also emits the QCoreApplication::aboutToQuit() signal. This signal is used for cleanup code.
     */
    QCoreApplication::exit(0);
}

// Construction
QEGui::QEGui(int& argc, char **argv ) : QApplication( argc, argv )
{
    qRegisterMetaType<QEForm*>( "QEForm*" );   // must also register declared meta types.
    loginForm = NULL;
}

// Run the application
int QEGui::run()
{
    // Get the startup parameters from the command line arguments
    // Just print a message if required (bad parameters, or only or versionversion info required)
    QStringList args = QCoreApplication::arguments();
    bool argsAreOkay = params.getStartupParams( args );

    if (!argsAreOkay)
    {
        printUsage (std::cerr);
        return 1;
    }

    if (params.printHelp)
    {
       printHelp ();
       return 0;
    }

    if (params.printVersion)
    {
       printVersion ();
       return 0;
    }


    // Restore the user level passwords
    QSettings settings( "epicsqt", "QEGui");
    setUserLevelPassword( userLevelTypes::USERLEVEL_USER, settings.value( "userPassword" ).toString() );
    setUserLevelPassword( userLevelTypes::USERLEVEL_SCIENTIST, settings.value( "scientistPassword" ).toString() );
    setUserLevelPassword( userLevelTypes::USERLEVEL_ENGINEER, settings.value( "engineerPassword" ).toString() );

    // Restore recent files
    int i;
    bool ok;
    int recentFilesCount = settings.value( "recentFileCount" ).toInt( &ok );
    if( ok )
    {
        for( i = 0; i < recentFilesCount; i++ )
        {
            QString name = settings.value( QString( "recentFileName%1" ).arg( i )).toString();
            QString path = settings.value( QString( "recentFilePath%1" ).arg( i )).toString();
            QStringList pathList = settings.value( QString( "recentFilePathList%1" ).arg( i )).toStringList();
            QString macroSubstitutions = settings.value( QString( "recentFileMacroSubstitutions%1" ).arg( i )).toString();
            QString customisationName = settings.value( QString( "recentCustomisationName%1" ).arg( i )).toString();
            recentFiles.append( new recentFile( name, path, pathList, macroSubstitutions, customisationName, this ));
        }
    }

    // Set up the profile for finding customisation files, and for loading customisations
    ContainerProfile profile;
    profile.setupProfile( NULL, params.pathList, "", params.substitutions );

    // Load window customisations
    // First load the inbuilt default
    // This can be overwritten by any external file with a customisation set with the name defined by DEFAULT_QEGUI_CUSTOMISATION
    winCustomisations.loadCustomisation( ":/qe/gui/configuration/QEGuiCustomisationDefault.xml" );

    // Now load the configuration file specified in the parameters (if any), otherwise the default external file if present
    if( !winCustomisations.loadCustomisation( getParams()->customisationFile ))
    {
        QString defaultCustomisationName( "QEGuiCustomisation.xml" );
        QFile file( defaultCustomisationName );
        if( file.exists() )
        {
            winCustomisations.loadCustomisation( defaultCustomisationName );
        }
    }

    // If there were any errors loading customisations, log the customisations
    if( winCustomisations.log.getError() )
    {
        qDebug() << "Window customisation errors. The log is being written to customisationErrors.log";
        QMessageBox msgBox;
        msgBox.setText("Window customisation errors. The log is being written to customisationErrors.log");
        msgBox.exec();

        QString log = winCustomisations.log.getLog();
        QFile errorLogFile( "customisationErrors.log" );
        if( errorLogFile.open(QFile::WriteOnly | QFile::Truncate) )
        {
            QTextStream errorLog( &errorLogFile );
            errorLog << QString( "QEGui customisation log   " );
            errorLog << QDateTime::currentDateTime().toString();
            errorLog << "\n\nAn error occured trying to prepare customisations for QEGui. Search for ERROR:\n\n";
            errorLog << log;
            errorLogFile.close();
        }

    }

    // Release the profile used while looking for customisation files
    profile.releaseProfile();

    // Prepare to manage save and restore
    // Note, main windows look after themselves, this is for the overall application
    saveRestoreManager saveRestore( this );

    // If only a single instance has been requested,
    // and if there is already another instance of QEGui
    // and it takes the parameters, do no more
    instanceManager instance( this );
    if( params.singleApp && instance.handball( &params ) )
        return 0;

    // Define application scaling to be applied to all widgets.
    // Recall adjustScale is expressed as a percentage.
    //
    QEScaling::setScaling( int( params.adjustScale ), 100 );

    // Start automatic saving of current configuration
    startAutoSaveConfig( params.configurationFile, params.disableAutoSaveConfiguration );

    // Start the main application window
    instance.newWindow( params );
    int ret = exec();

    // Save passwords
    settings.setValue( "userPassword", getUserLevelPassword( userLevelTypes::USERLEVEL_USER ));
    settings.setValue( "scientistPassword", getUserLevelPassword( userLevelTypes::USERLEVEL_SCIENTIST ));
    settings.setValue( "engineerPassword", getUserLevelPassword( userLevelTypes::USERLEVEL_ENGINEER ));

    // Save recent files
    settings.setValue( "recentFileCount", recentFiles.count() );
    for( i = 0; i < recentFiles.count(); i++ )
    {
        settings.setValue( QString( "recentFileName%1" ).arg( i ), recentFiles.at( i )->name );
        settings.setValue( QString( "recentFilePath%1" ).arg( i ), recentFiles.at( i )->path );
        settings.setValue( QString( "recentFilePathList%1" ).arg( i ), recentFiles.at( i )->pathList );
        settings.setValue( QString( "recentFileMacroSubstitutions%1" ).arg( i ), recentFiles.at( i )->macroSubstitutions );
        settings.setValue( QString( "recentCustomisationName%1" ).arg( i ), recentFiles.at( i )->customisationName );
    }

    if (signal(SIGINT, unixSignalHandler) == SIG_ERR) {
        qFatal("ERR - %s(%d): An error occurred while setting a signal handler.\n", __FILE__,__LINE__);
    }
    if (signal(SIGTERM, unixSignalHandler) == SIG_ERR) {
        qFatal("ERR - %s(%d): An error occurred while setting a signal handler.\n", __FILE__,__LINE__);
    }

    return ret;
}

// Print version info
void QEGui::printVersion ()
{
   std::cout  << "QEGui version:     " << QE_VERSION_STRING << "  "
              << QE_VERSION_DATE_TIME <<  " (using QT " << QT_VERSION_STR << ")" << std::endl;

   std::cout  << "Framework version: "
              << QEFrameworkVersion::getString().toLatin1().data()       << "  "
              << QEFrameworkVersion::getDateTime().toLatin1().data()     << " (using QT "
              << QEFrameworkVersion::getQtVersionStr().toLatin1().data() << ")" << std::endl;

   // Note: the EPICS version string is prefixed by the text "EPICS" and
   // the QWT version string is prefixed by "QWT"
   //
   std::cout  << "Support packages:  " << QEFrameworkVersion::getEpicsVersionStr ().toLatin1().data()
              << " and "    << QEFrameworkVersion::getQwtVersionStr().toLatin1().data() << std::endl;
}

// Print file to stream
void QEGui::printFile (const QString& filename,
                       std::ostream& stream)
{
   QFile textFile (filename);

   if (!textFile.open (QIODevice::ReadOnly | QIODevice::Text)) {
      return;
   }

   QTextStream textStream( &textFile );
   QString text = textStream.readAll();
   textFile.close();

   stream << text.toLatin1().data();
}

// Print command line usage
void QEGui::printUsage (std::ostream& stream)
{
   printFile (":/qe/gui/help/help_usage.txt", stream);
}

// Prinf command line help
void QEGui::printHelp ()
{
   printVersion();
   std::cout << "\n";
   printUsage( std::cout );
   printFile( ":/qe/gui/help/help_general.txt", std::cout );
}

// Get the application's startup parameters
startupParams* QEGui::getParams()
{
    return &params;
}

// Get the number of main windows
int QEGui::getMainWindowCount()
{
    return mainWindowList.count();
}

// Get the main window given an index into the application's list of main windows
// Return NULL if past the end of the list
MainWindow* QEGui::getMainWindow( int i )
{
    if( i >= mainWindowList.count() )
    {
        return NULL;
    }

    return mainWindowList[i];
}

// Locate a main window in the application's list of main windows
int QEGui::getMainWindowPosition( MainWindow* mw )
{
    for( int i = 0; i < mainWindowList.count(); i++ )
    {
        if( mainWindowList[i] == mw )
            return i;
    }

    // Should never get here
    return 0;
}

// Add a main window to the application's list of main windows
void QEGui::addMainWindow( MainWindow* window )
{
    mainWindowList.append( window );
}

// Remove a main window from the application's list of main windows given a reference to the main window
void QEGui::removeMainWindow( MainWindow* window )
{
    // Remove this main window from the global list of main windows
    // Note, this may have already been done to hide the the main window if deleting using deleteLater()
    for( int i = 0; i < mainWindowList.size(); ++i )
    {
        if( mainWindowList[i] == window )
        {
            mainWindowList.removeAt( i );
            break;
        }
    }
}

// Remove a main window from the application's list of main windows given an index into the application's list of main windows
void QEGui::removeMainWindow( int i )
{
    mainWindowList.removeAt( i );
}

// Return list of recently added files
const QList<recentFile*>&  QEGui::getRecentFiles()
{
    return recentFiles;
}

// If a GUI matching a filename and macro substitutions is present, ensure it is visible and has focus.
// Return true if GUI is found
 MainWindow* QEGui::raiseGui(  QString guiFileName, QString macroSubstitutions, QString title )
{
    for( int i = 0; i < mainWindowList.count(); i++ )
    {
        MainWindow* mw = mainWindowList[i];

        // If the guifileName and macro substitution matches, ensure the specific GUI
        // in the main window is displayed
        if( mw->showGui( guiFileName, macroSubstitutions ) )
        {
            return mw;
        }

        // If the main window title matches, then show it
        else
        {
            VariableNameManager vnm;
            vnm.setVariableNameSubstitutions( macroSubstitutions );
            QString substitutedTitle = vnm.substituteThis( title );
            if( mw->windowTitle() == substitutedTitle )
            {
                mw->setWindowState((mw->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
                mw->show();
                mw->raise();
                mw->activateWindow();
                return mw;
            }
        }

    }
    return NULL;
}

// Add a GUI to the application's list of GUIs, and to the recent menu
void QEGui::addGui( QEForm* gui, QString customisationName )
{
    // Note the GUI title and full file path
    QString name = gui->getQEGuiTitle();
    QString path = gui->getFullFileName();

    // Assume there is no 'Recent' action
    QAction* recentMenuAction = NULL;

    // Look for the gui in the recent files list
    for( int i = 0; i < recentFiles.count(); i++ )
    {
        // If already in the list, promote the found entry to the top of the list
        if( name == recentFiles[i]->name && path == recentFiles[i]->path )
        {
            // Note the found action
            recentMenuAction = recentFiles[i];

            // Promote the action to the top of all the menus it is in
            QList<QWidget*> assocWidgets = recentMenuAction->associatedWidgets();
            for( int j = 0; j < assocWidgets.count(); j++ )
            {
                QWidget* menu = assocWidgets[j];
                menu->removeAction( recentMenuAction );
                QAction* beforeAction = 0;
                if( menu->actions().count() )
                {
                    beforeAction = menu->actions().at(0);
                }
                menu->insertAction( beforeAction, recentMenuAction );
            }

            // Promote the recent file info in the recent file list
            recentFiles.prepend( recentFiles.takeAt( i ) );

            break;
        }
    }

    // If the current gui was not found in the recent file list, add it
    if( !recentMenuAction )
    {
        // Add a new recent gui
        recentFile* rf = new recentFile( name, path, gui->getPathList(), gui->getMacroSubstitutions(), customisationName, this );
        recentFiles.prepend( rf );

        // Keep the list down to a limited size
        if( recentFiles.count() > 10 )
        {
            // Deleting the action will remove it from all the menus it is in
            delete( recentFiles.takeLast() );
        }

        // For each main window, add the recent file
        for( int i = 0; i < mainWindowList.count(); i++ )
        {
            mainWindowList[i]->addRecentMenuAction( rf );
        }
    }
}

// Change user level
void QEGui::login( QWidget* fromForm )
{
    if( !loginForm )
    {
        loginForm = new loginDialog;
        // Ensure scaling is consistent with the rest of the application's forms.
        QEScaling::applyToWidget( loginForm );
    }

    loginForm->exec( fromForm );
}

// Launch a gui for the 'Recent...' menu
void QEGui::launchRecentGui( QString path, QStringList pathList, QString macroSubstitutions, QString customisationName )
{
    // Set up the profile for the new window
    ContainerProfile profile;

    profile.setupProfile( NULL, pathList, "", macroSubstitutions );

    MainWindow* mw = new MainWindow( this, path, "", customisationName, QEFormMapper::nullHandle(), false );
    mw->show();
    profile.releaseProfile();
}

// Save the current configuration.
//
// This may be called as a result of the user requesting to save the configuration, in which
// case the user will have chosen the configuration name.
// This may also be called as part of auto-saving the configuration.
//
// Note,  the Persistance Manager is pased in, as it is likely to have just been used by the caller
void QEGui::saveConfiguration( PersistanceManager* pm,        // Persistance manager
                               const QString configFile,      // Configuration file name
                               const QString rootName,        // XML root name
                               const QString configName,      // Configuration name
                               const bool warnUser )          // True if this is interactive, in which case user will be notified of errors
{
    // Give all main windows and top level QEForms (managed by this application) a unique identifier required for restoration
    int i = 0;
    MainWindow* mw;
    while( (mw = getMainWindow( i )) )
    {
        mw->identifyWindowAndForms( i );

        // Next main window
        i++;
    }

    // Ask the persistance manager to save the current configuration.
    // The persistance manager will signal all interested objects (including this application) that
    // they should present anything they wish to save.
    pm->save( configFile, rootName, configName, warnUser );
}

// end
