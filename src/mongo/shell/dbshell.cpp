// dbshell.cpp
/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include <stdio.h>
#include <string.h>

#include "../third_party/linenoise/linenoise.h"
#include "../scripting/engine.h"
#include "../client/dbclient.h"
#include "../util/unittest.h"
#include "../db/cmdline.h"
#include "utils.h"
#include "../util/password.h"
#include "../util/version.h"
#include "../util/goodies.h"
#include "../util/file.h"
#include "../db/repl/rs_member.h"

#include <boost/filesystem/operations.hpp>

using namespace std;
using namespace mongo;

string historyFile;
bool gotInterrupted = false;
bool inMultiLine = false;
static volatile bool atPrompt = false; // can eval before getting to prompt
bool autoKillOp = false;

#if !defined(__freebsd__) && !defined(__openbsd__) && !defined(_WIN32)
// this is for ctrl-c handling
#include <setjmp.h>
jmp_buf jbuf;
#endif

namespace mongo {

    Scope * shellMainScope;

    extern bool dbexitCalled;
}

void generateCompletions( const string& prefix , vector<string>& all ) {
    if ( prefix.find( '"' ) != string::npos )
        return;

    try {
        BSONObj args = BSON( "0" << prefix );
        shellMainScope->invokeSafe( "function callShellAutocomplete(x) {shellAutocomplete(x)}", &args, 0, 1000 );
        BSONObjBuilder b;
        shellMainScope->append( b , "" , "__autocomplete__" );
        BSONObj res = b.obj();
        BSONObj arr = res.firstElement().Obj();

        BSONObjIterator i( arr );
        while ( i.more() ) {
            BSONElement e = i.next();
            all.push_back( e.String() );
        }
    }
    catch ( ... ) {
    }
}

void completionHook( const char* text , linenoiseCompletions* lc ) {
    vector<string> all;
    generateCompletions( text , all );

    for ( unsigned i = 0; i < all.size(); ++i )
        linenoiseAddCompletion( lc , (char*)all[i].c_str() );
}

void shellHistoryInit() {
    stringstream ss;
    const char * h = shellUtils::getUserDir();
    if ( h )
        ss << h << "/";
    ss << ".dbshell";
    historyFile = ss.str();

    linenoiseHistoryLoad( historyFile.c_str() );
    linenoiseSetCompletionCallback( completionHook );
}

void shellHistoryDone() {
    linenoiseHistorySave( historyFile.c_str() );
    linenoiseHistoryFree();
}
void shellHistoryAdd( const char * line ) {
    if ( line[0] == '\0' )
        return;

    // dont record duplicate lines
    static string lastLine;
    if ( lastLine == line )
        return;
    lastLine = line;

    if ( strstr( line, ".auth") == NULL )
        linenoiseHistoryAdd( line );
}

#ifdef CTRLC_HANDLE
void intr( int sig ) {
    longjmp( jbuf , 1 );
}
#endif

void killOps() {
    if ( mongo::shellUtils::_nokillop || mongo::shellUtils::_allMyUris.size() == 0 )
        return;

    if ( atPrompt )
        return;

    sleepmillis(10); // give current op a chance to finish

    for( map< string, set<string> >::const_iterator i = shellUtils::_allMyUris.begin(); i != shellUtils::_allMyUris.end(); ++i ) {
        string errmsg;
        ConnectionString cs = ConnectionString::parse( i->first, errmsg );
        if (!cs.isValid()) continue;
        boost::scoped_ptr<DBClientWithCommands> conn( cs.connect( errmsg ) );
        if (!conn) continue;

        const set<string>& uris = i->second;

        BSONObj inprog =  conn->findOne( "admin.$cmd.sys.inprog", Query() )["inprog"].embeddedObject().getOwned();
        BSONForEach( op, inprog ) {
            if ( uris.count( op["client"].String() ) ) {
                ONCE if ( !autoKillOp ) {
                    cout << endl << "do you want to kill the current op(s) on the server? (y/n): ";
                    cout.flush();

                    char yn;
                    cin >> yn;

                    if ( yn != 'y' && yn != 'Y' )
                        return;
                }

                conn->findOne( "admin.$cmd.sys.killop", QUERY( "op"<< op["opid"] ) );
            }
        }
    }
}

void quitNicely( int sig ) {
    mongo::dbexitCalled = true;
    if ( sig == SIGINT && inMultiLine ) {
        gotInterrupted = 1;
        return;
    }

#if !defined(_WIN32)
    if ( sig == SIGPIPE )
        mongo::rawOut( "mongo got signal SIGPIPE\n" );
#endif

    killOps();
    shellHistoryDone();
    exit(0);
}

// the returned string is allocated with strdup() or malloc() and must be freed by calling free()
char * shellReadline( const char * prompt , int handlesigint = 0 ) {
    atPrompt = true;

#ifdef CTRLC_HANDLE
    if ( ! handlesigint ) {
        char* ret = linenoise( prompt );
        atPrompt = false;
        return ret;
    }
    if ( setjmp( jbuf ) ) {
        gotInterrupted = 1;
        sigrelse(SIGINT);
        signal( SIGINT , quitNicely );
        return 0;
    }
    signal( SIGINT , intr );
#endif

    char * ret = linenoise( prompt );
    if ( ! ret ) {
        gotInterrupted = true;  // got ^C, break out of multiline
    }

    signal( SIGINT , quitNicely );
    atPrompt = false;
    return ret;
}

#ifdef _WIN32
char * strsignal(int sig){
    switch (sig){
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGABRT: return "SIGABRT";
        case SIGSEGV: return "SIGSEGV";
        case SIGFPE: return "SIGFPE";
        default: return "unknown";
    }
}
#endif

void quitAbruptly( int sig ) {
    ostringstream ossSig;
    ossSig << "mongo got signal " << sig << " (" << strsignal( sig ) << "), stack trace: " << endl;
    mongo::rawOut( ossSig.str() );

    ostringstream ossBt;
    mongo::printStackTrace( ossBt );
    mongo::rawOut( ossBt.str() );

    mongo::shellUtils::KillMongoProgramInstances();
    exit( 14 );
}

// this will be called in certain c++ error cases, for example if there are two active
// exceptions
void myterminate() {
    mongo::rawOut( "terminate() called in shell, printing stack:" );
    mongo::printStackTrace();
    exit( 14 );
}

void setupSignals() {
    signal( SIGINT , quitNicely );
    signal( SIGTERM , quitNicely );
    signal( SIGABRT , quitAbruptly );
    signal( SIGSEGV , quitAbruptly );
    signal( SIGFPE , quitAbruptly );

#if !defined(_WIN32) // surprisingly these are the only ones that don't work on windows
    signal( SIGPIPE , quitNicely ); // Maybe just log and continue?
    signal( SIGBUS , quitAbruptly );
#endif

    set_terminate( myterminate );
}

string fixHost( string url , string host , string port ) {
    //cout << "fixHost url: " << url << " host: " << host << " port: " << port << endl;

    if ( host.size() == 0 && port.size() == 0 ) {
        if ( url.find( "/" ) == string::npos ) {
            // check for ips
            if ( url.find( "." ) != string::npos )
                return url + "/test";

            if ( url.rfind( ":" ) != string::npos &&
                    isdigit( url[url.rfind(":")+1] ) )
                return url + "/test";
        }
        return url;
    }

    if ( url.find( "/" ) != string::npos ) {
        cerr << "url can't have host or port if you specify them individually" << endl;
        exit(-1);
    }

    if ( host.size() == 0 )
        host = "127.0.0.1";

    string newurl = host;
    if ( port.size() > 0 )
        newurl += ":" + port;
    else if ( host.find(':') == string::npos ) {
        // need to add port with IPv6 addresses
        newurl += ":27017";
    }

    newurl += "/" + url;

    return newurl;
}

static string OpSymbols = "~!%^&*-+=|:,<>/?.";

bool isOpSymbol( char c ) {
    for ( size_t i = 0; i < OpSymbols.size(); i++ )
        if ( OpSymbols[i] == c ) return true;
    return false;
}

bool isUseCmd( string code ) {
    string cmd = code;
    if ( cmd.find( " " ) > 0 )
        cmd = cmd.substr( 0 , cmd.find( " " ) );
    return cmd == "use";
}

bool isBalanced( string code ) {
    if (isUseCmd( code ))
        return true;  // don't balance "use <dbname>" in case dbname contains special chars
    int brackets = 0;
    int parens = 0;
    bool danglingOp = false;

    for ( size_t i=0; i<code.size(); i++ ) {
        switch( code[i] ) {
        case '/':
            if ( i + 1 < code.size() && code[i+1] == '/' ) {
                while ( i  <code.size() && code[i] != '\n' )
                    i++;
            }
            continue;
        case '{': brackets++; break;
        case '}': if ( brackets <= 0 ) return true; brackets--; break;
        case '(': parens++; break;
        case ')': if ( parens <= 0 ) return true; parens--; break;
        case '"':
            i++;
            while ( i < code.size() && code[i] != '"' ) i++;
            break;
        case '\'':
            i++;
            while ( i < code.size() && code[i] != '\'' ) i++;
            break;
        case '\\':
            if ( i + 1 < code.size() && code[i+1] == '/' ) i++;
            break;
        case '+':
        case '-':
            if ( i + 1 < code.size() && code[i+1] == code[i] ) {
                i++;
                continue; // postfix op (++/--) can't be a dangling op
            }
            break;
        }
        if ( i >= code.size() ) {
            danglingOp = false;
            break;
        }
        if ( isOpSymbol( code[i] ) ) danglingOp = true;
        else if ( !std::isspace( static_cast<unsigned char>( code[i] ) ) ) danglingOp = false;
    }

    return brackets == 0 && parens == 0 && !danglingOp;
}

using mongo::asserted;

struct BalancedTest : public mongo::UnitTest {
public:
    void run() {
        assert( isBalanced( "x = 5" ) );
        assert( isBalanced( "function(){}" ) );
        assert( isBalanced( "function(){\n}" ) );
        assert( ! isBalanced( "function(){" ) );
        assert( isBalanced( "x = \"{\";" ) );
        assert( isBalanced( "// {" ) );
        assert( ! isBalanced( "// \n {" ) );
        assert( ! isBalanced( "\"//\" {" ) );
        assert( isBalanced( "{x:/x\\//}" ) );
        assert( ! isBalanced( "{ \\/// }" ) );
        assert( isBalanced( "x = 5 + y ") );
        assert( ! isBalanced( "x = ") );
        assert( ! isBalanced( "x = // hello") );
        assert( ! isBalanced( "x = 5 +") );
        assert( isBalanced( " x ++") );
        assert( isBalanced( "-- x") );
        assert( !isBalanced( "a.") );
        assert( !isBalanced( "a. ") );
        assert( isBalanced( "a.b") );
    }
} balanced_test;

string finishCode( string code ) {
    while ( ! isBalanced( code ) ) {
        inMultiLine = true;
        code += "\n";
        // cancel multiline if two blank lines are entered
        if ( code.find( "\n\n\n" ) != string::npos )
            return ";";
        char * line = shellReadline( "... " , 1 );
        if ( gotInterrupted ) {
            if ( line )
                free( line );
            return "";
        }
        if ( ! line )
            return "";

        while ( startsWith( line, "... " ) )
            line += 4;

        code += line;
        free( line );
    }
    return code;
}

#include <boost/program_options.hpp>
namespace po = boost::program_options;

void show_help_text( const char* name, po::options_description options ) {
    cout << "MongoDB shell version: " << mongo::versionString << endl;
    cout << "usage: " << name << " [options] [db address] [file names (ending in .js)]" << endl
         << "db address can be:" << endl
         << "  foo                   foo database on local machine" << endl
         << "  192.169.0.5/foo       foo database on 192.168.0.5 machine" << endl
         << "  192.169.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999" << endl
         << options << endl
         << "file names: a list of files to run. files have to end in .js and will exit after "
         << "unless --shell is specified" << endl;
};

bool fileExists( string file ) {
    try {
        boost::filesystem::path p( file );
        return boost::filesystem::exists( file );
    }
    catch ( ... ) {
        return false;
    }
}

namespace mongo {
    extern bool isShell;
    extern DBClientWithCommands *latestConn;
}

string sayReplSetMemberState() {
    try {
        if( latestConn ) {
            BSONObj info;
            if( latestConn->runCommand( "admin", BSON( "replSetGetStatus" << 1 << "forShell" << 1 ) , info ) ) {
                stringstream ss;
                ss << info["set"].String() << ':';
                
                int s = info["myState"].Int();
                MemberState ms( s );
                ss << ms.toString();

                return ss.str();
            }
            else {
                string s = info.getStringField( "info" );
                if( s.size() < 20 )
                    return s; // "mongos", "configsvr"
            }
        }
    }
    catch( std::exception& e ) {
        log( 1 ) << "error in sayReplSetMemberState:" << e.what() << endl;
    }
    return "";
}

/**
 * Edit a variable in an external editor -- EDITOR must be defined
 *
 * @param var Name of JavaScript variable to be edited
 */
static void edit( const string& var ) {

    // EDITOR must be defined in the environment
    static const char * editor = getenv( "EDITOR" );
    if ( !editor ) {
        cout << "please define the EDITOR environment variable" << endl;
        return;
    }

    // "var" must look like a variable/property name
    for ( const char* p=var.c_str(); *p; ++p ) {
        if ( ! ( isalnum( *p ) || *p == '_' || *p == '.' ) ) {
            cout << "can only edit variable or property" << endl;
            return;
        }
    }

    // Convert "var" to JavaScript (JSON) text
    if ( !shellMainScope->exec( "__jsout__ = tojson(" + var + ")", "tojs", false, false, false ) )
        return; // Error already printed

    const string js = shellMainScope->getString( "__jsout__" );

    if ( strstr( js.c_str(), "[native code]" ) ) {
        cout << "can't edit native functions" << endl;
        return;
    }

    // Pick a name to use for the temp file
    string filename;
    const int maxAttempts = 10;
    int i;
    for ( i = 0; i < maxAttempts; ++i ) {
        StringBuilder sb;
#ifdef _WIN32
        char tempFolder[MAX_PATH];
        GetTempPathA( sizeof tempFolder, tempFolder );
        sb << tempFolder << "mongo_edit" << time( 0 ) + i << ".js";
#else
        sb << "/tmp/mongo_edit" << time( 0 ) + i << ".js";
#endif
        filename = sb.str();
        if ( ! fileExists( filename ) )
            break;
    }
    if ( i == maxAttempts ) {
        cout << "couldn't create unique temp file after " << maxAttempts << " attempts" << endl;
        return;
    }

    // Create the temp file
    FILE * tempFileStream;
    tempFileStream = fopen( filename.c_str(), "wt" );
    if ( ! tempFileStream ) {
        cout << "couldn't create temp file (" << filename << "): " << errnoWithDescription() << endl;
        return;
    }

    // Write JSON into the temp file
    size_t fileSize = js.size();
    if ( fwrite( js.data(), sizeof( char ), fileSize, tempFileStream ) != fileSize ) {
        int systemErrno = errno;
        cout << "failed to write to temp file: " << errnoWithDescription( systemErrno ) << endl;
        fclose( tempFileStream );
        remove( filename.c_str() );
        return;
    }
    fclose( tempFileStream );

    // Pass file to editor
    StringBuilder sb;
    sb << editor << " " << filename;
    int ret = ::system( sb.str().c_str() );
    if ( ret ) {
        if ( ret == -1 ) {
            int systemErrno = errno;
            cout << "failed to launch $EDITOR (" << editor << "): " << errnoWithDescription( systemErrno ) << endl;
        }
        else
            cout << "editor exited with error (" << ret << "), not applying changes" << endl;
        remove( filename.c_str() );
        return;
    }

    // The editor gave return code zero, so read the file back in
    tempFileStream = fopen( filename.c_str(), "rt" );
    if ( ! tempFileStream ) {
        cout << "couldn't open temp file on return from editor: " << errnoWithDescription() << endl;
        remove( filename.c_str() );
        return;
    }
    sb.reset();
    sb << var << " = ";
    int bytes;
    do {
        char buf[1024];
        bytes = fread( buf, sizeof( char ), sizeof buf, tempFileStream );
        if ( ferror( tempFileStream ) ) {
            cout << "failed to read temp file: " << errnoWithDescription() << endl;
            fclose( tempFileStream );
            remove( filename.c_str() );
            return;
        }
        sb.append( StringData( buf, bytes ) );
    } while ( bytes );

    // Done with temp file, close and delete it
    fclose( tempFileStream );
    remove( filename.c_str() );

    // Try to execute assignment to copy edited value back into the variable
    const string code = sb.str();
    if ( !shellMainScope->exec( code, "tojs", false, false, false ) )
        return; // Error already printed
}

int _main( int argc, char* argv[] ) {
    mongo::isShell = true;
    setupSignals();

    mongo::shellUtils::RecordMyLocation( argv[ 0 ] );

    string url = "test";
    string dbhost;
    string port;
    vector<string> files;

    string username;
    string password;

    bool runShell = false;
    bool nodb = false;
    bool norc = false;

    string script;

    po::options_description shell_options( "options" );
    po::options_description hidden_options( "Hidden options" );
    po::options_description cmdline_options( "Command line options" );
    po::positional_options_description positional_options;

    shell_options.add_options()
    ( "shell", "run the shell after executing files" )
    ( "nodb", "don't connect to mongod on startup - no 'db address' arg expected" )
    ( "norc", "will not run the \".mongorc.js\" file on start up" )
    ( "quiet", "be less chatty" )
    ( "port", po::value<string>( &port ), "port to connect to" )
    ( "host", po::value<string>( &dbhost ), "server to connect to" )
    ( "eval", po::value<string>( &script ), "evaluate javascript" )
    ( "username,u", po::value<string>(&username), "username for authentication" )
    ( "password,p", new mongo::PasswordValue( &password ), "password for authentication" )
    ( "help,h", "show this usage information" )
    ( "version", "show version information" )
    ( "verbose", "increase verbosity" )
    ( "ipv6", "enable IPv6 support (disabled by default)" )
#ifdef MONGO_SSL
    ( "ssl", "use all for connections" )
#endif
    ;

    hidden_options.add_options()
    ( "dbaddress", po::value<string>(), "dbaddress" )
    ( "files", po::value< vector<string> >(), "files" )
    ( "nokillop", "nokillop" ) // for testing, kill op will also be disabled automatically if the tests starts a mongo program
    ( "autokillop", "autokillop" ) // for testing, will kill op without prompting
    ;

    positional_options.add( "dbaddress", 1 );
    positional_options.add( "files", -1 );

    cmdline_options.add( shell_options ).add( hidden_options );

    po::variables_map params;

    /* using the same style as db.cpp uses because eventually we're going
     * to merge some of this stuff. */
    int command_line_style = (((po::command_line_style::unix_style ^
                                po::command_line_style::allow_guessing) |
                               po::command_line_style::allow_long_disguise) ^
                              po::command_line_style::allow_sticky);

    try {
        po::store(po::command_line_parser(argc, argv).options(cmdline_options).
                  positional(positional_options).
                  style(command_line_style).run(), params);
        po::notify( params );
    }
    catch ( po::error &e ) {
        cout << "ERROR: " << e.what() << endl << endl;
        show_help_text( argv[0], shell_options );
        return mongo::EXIT_BADOPTIONS;
    }

    // hide password from ps output
    for ( int i = 0; i < (argc-1); ++i ) {
        if ( !strcmp(argv[i], "-p") || !strcmp( argv[i], "--password" ) ) {
            char* arg = argv[i + 1];
            while ( *arg ) {
                *arg++ = 'x';
            }
        }
    }

    if ( params.count( "shell" ) ) {
        runShell = true;
    }
    if ( params.count( "nodb" ) ) {
        nodb = true;
    }
    if ( params.count( "norc" ) ) {
        norc = true;
    }
    if ( params.count( "help" ) ) {
        show_help_text( argv[0], shell_options );
        return mongo::EXIT_CLEAN;
    }
    if ( params.count( "files" ) ) {
        files = params["files"].as< vector<string> >();
    }
    if ( params.count( "version" ) ) {
        cout << "MongoDB shell version: " << mongo::versionString << endl;
        return mongo::EXIT_CLEAN;
    }
    if ( params.count( "quiet" ) ) {
        mongo::cmdLine.quiet = true;
    }
#ifdef MONGO_SSL
    if ( params.count( "ssl" ) ) {
        mongo::cmdLine.sslOnNormalPorts = true;
    }
#endif
    if ( params.count( "nokillop" ) ) {
        mongo::shellUtils::_nokillop = true;
    }
    if ( params.count( "autokillop" ) ) {
        autoKillOp = true;
    }

    /* This is a bit confusing, here are the rules:
     *
     * if nodb is set then all positional parameters are files
     * otherwise the first positional parameter might be a dbaddress, but
     * only if one of these conditions is met:
     *   - it contains no '.' after the last appearance of '\' or '/'
     *   - it doesn't end in '.js' and it doesn't specify a path to an existing file */
    if ( params.count( "dbaddress" ) ) {
        string dbaddress = params["dbaddress"].as<string>();
        if (nodb) {
            files.insert( files.begin(), dbaddress );
        }
        else {
            string basename = dbaddress.substr( dbaddress.find_last_of( "/\\" ) + 1 );
            if (basename.find_first_of( '.' ) == string::npos ||
                    ( basename.find( ".js", basename.size() - 3 ) == string::npos && !fileExists( dbaddress ) ) ) {
                url = dbaddress;
            }
            else {
                files.insert( files.begin(), dbaddress );
            }
        }
    }
    if ( params.count( "ipv6" ) ) {
        mongo::enableIPv6();
    }
    if ( params.count( "verbose" ) ) {
        logLevel = 1;
    }

    if ( url == "*" ) {
        cout << "ERROR: " << "\"*\" is an invalid db address" << endl << endl;
        show_help_text( argv[0], shell_options );
        return mongo::EXIT_BADOPTIONS;
    }

    if ( ! mongo::cmdLine.quiet )
        cout << "MongoDB shell version: " << mongo::versionString << endl;

    mongo::UnitTest::runTests();

    if ( !nodb ) { // connect to db
        //if ( ! mongo::cmdLine.quiet ) cout << "url: " << url << endl;

        stringstream ss;
        if ( mongo::cmdLine.quiet )
            ss << "__quiet = true;";
        ss << "db = connect( \"" << fixHost( url , dbhost , port ) << "\")";

        mongo::shellUtils::_dbConnect = ss.str();

        if ( params.count( "password" ) && password.empty() )
            password = mongo::askPassword();

        if ( username.size() && password.size() ) {
            stringstream ss;
            ss << "if ( ! db.auth( \"" << username << "\" , \"" << password << "\" ) ){ throw 'login failed'; }";
            mongo::shellUtils::_dbAuth = ss.str();
        }
    }

    mongo::ScriptEngine::setConnectCallback( mongo::shellUtils::onConnect );
    mongo::ScriptEngine::setup();
    mongo::globalScriptEngine->setScopeInitCallback( mongo::shellUtils::initScope );
    auto_ptr< mongo::Scope > scope( mongo::globalScriptEngine->newScope() );
    shellMainScope = scope.get();

    if( runShell )
        cout << "type \"help\" for help" << endl;

    if ( !script.empty() ) {
        mongo::shellUtils::MongoProgramScope s;
        if ( ! scope->exec( script , "(shell eval)" , true , true , false ) )
            return -4;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        mongo::shellUtils::MongoProgramScope s;

        if ( files.size() > 1 )
            cout << "loading file: " << files[i] << endl;

        if ( ! scope->execFile( files[i] , false , true , false ) ) {
            cout << "failed to load: " << files[i] << endl;
            return -3;
        }
    }

    if ( files.size() == 0 && script.empty() )
        runShell = true;

    if ( runShell ) {

        mongo::shellUtils::MongoProgramScope s;

        if ( !norc ) {
            string rcLocation;
#ifndef _WIN32
            if ( getenv( "HOME" ) != NULL )
                rcLocation = str::stream() << getenv( "HOME" ) << "/.mongorc.js" ;
#else
            if ( getenv( "HOMEDRIVE" ) != NULL && getenv( "HOMEPATH" ) != NULL )
                rcLocation = str::stream() << getenv( "HOMEDRIVE" ) << getenv( "HOMEPATH" ) << "\\.mongorc.js";
#endif
            if ( !rcLocation.empty() && fileExists(rcLocation) ) {
                if ( ! scope->execFile( rcLocation , false , true , false , 0 ) ) {
                    cout << "The \".mongorc.js\" file located in your home folder could not be executed" << endl;
                    return -5;
                }
            }
        }

        shellHistoryInit();

        string prompt;
        int promptType; 

        //v8::Handle<v8::Object> shellHelper = baseContext_->Global()->Get( v8::String::New( "shellHelper" ) )->ToObject();

        while ( 1 ) {
            inMultiLine = false;
            gotInterrupted = false;
//            shellMainScope->localConnect;
            //DBClientWithCommands *c = getConnection( JSContext *cx, JSObject *obj );

            bool haveStringPrompt = false;
            promptType = scope->type( "prompt" );
            if( promptType == String ) {
                prompt = scope->getString( "prompt" );
                haveStringPrompt = true;
            }
            else if( promptType == Code ) {
                scope->exec( "delete __prompt__;", "", false, false, false, 0 );
                scope->exec( "__prompt__ = prompt();", "", false, false, false, 0 );
                if( scope->type( "__prompt__" ) == String ) {
                    prompt = scope->getString( "__prompt__" );
                    haveStringPrompt = true;
                }
            }
            if( !haveStringPrompt )
                prompt = sayReplSetMemberState() + "> ";

            char * line = shellReadline( prompt.c_str() );

            char * linePtr = line;  // can't clobber 'line', we need to free() it later
            if ( linePtr ) {
                while ( linePtr[0] == ' ' )
                    ++linePtr;
                int lineLen = strlen( linePtr );
                while ( lineLen > 0 && linePtr[lineLen - 1] == ' ' )
                    linePtr[--lineLen] = 0;
            }

            if ( ! linePtr || ( strlen( linePtr ) == 4 && strstr( linePtr , "exit" ) ) ) {
                if ( ! mongo::cmdLine.quiet )
                    cout << "bye" << endl;
                if ( line )
                    free( line );
                break;
            }

            string code = linePtr;
            if ( code == "exit" || code == "exit;" ) {
                free( line );
                break;
            }
            if ( code == "cls" ) {
                free( line );
                linenoiseClearScreen();
                continue;
            }

            if ( code.size() == 0 ) {
                free( line );
                continue;
            }

            if ( startsWith( linePtr, "edit " ) ) {
                shellHistoryAdd( linePtr );

                const char* s = linePtr + 5; // skip "edit "
                while( *s && isspace( *s ) )
                    s++;

                edit( s );
                free( line );
                continue;
            }

            gotInterrupted = false;
            code = finishCode( code );
            if ( gotInterrupted ) {
                cout << endl;
                free( line );
                continue;
            }

            if ( code.size() == 0 ) {
                free( line );
                break;
            }

            bool wascmd = false;
            {
                string cmd = linePtr;
                if ( cmd.find( " " ) > 0 )
                    cmd = cmd.substr( 0 , cmd.find( " " ) );

                if ( cmd.find( "\"" ) == string::npos ) {
                    try {
                        scope->exec( (string)"__iscmd__ = shellHelper[\"" + cmd + "\"];" , "(shellhelp1)" , false , true , true );
                        if ( scope->getBoolean( "__iscmd__" )  ) {
                            scope->exec( (string)"shellHelper( \"" + cmd + "\" , \"" + code.substr( cmd.size() ) + "\");" , "(shellhelp2)" , false , true , false );
                            wascmd = true;
                        }
                    }
                    catch ( std::exception& e ) {
                        cout << "error2:" << e.what() << endl;
                        wascmd = true;
                    }
                }
            }

            if ( ! wascmd ) {
                try {
                    if ( scope->exec( code.c_str() , "(shell)" , false , true , false ) )
                        scope->exec( "shellPrintHelper( __lastres__ );" , "(shell2)" , true , true , false );
                }
                catch ( std::exception& e ) {
                    cout << "error:" << e.what() << endl;
                }
            }

            shellHistoryAdd( code.c_str() );
            free( line );
        }

        shellHistoryDone();
    }

    mongo::dbexitCalled = true;
    return 0;
}

int main( int argc, char* argv[] ) {
    static mongo::StaticObserver staticObserver;
    try {
        return _main( argc , argv );
    }
    catch ( mongo::DBException& e ) {
        cerr << "exception: " << e.what() << endl;
        return -1;
    }
}
