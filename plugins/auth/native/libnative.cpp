/* -*- mode: c++; fill-column: 132; c-basic-offset: 4; indent-tabs-mode: nil -*- */

// =-=-=-=-=-=-=-
// irods includes
#include "rodsDef.h"
#include "msParam.h"
#include "reGlobalsExtern.h"
#include "rcConnect.h"
#include "authRequest.h"
#include "authResponse.h"
#include "authCheck.h"
#include "miscServerFunct.h"
#include "authPluginRequest.h"

// =-=-=-=-=-=-=-
// eirods includes
#include "eirods_auth_plugin.h"
#include "eirods_auth_constants.h"
#include "eirods_native_auth_object.h"
#include "eirods_stacktrace.h"
#include "eirods_kvp_string_parser.h"

// =-=-=-=-=-=-=-
// stl includes
#include <sstream>
#include <string>
#include <iostream>

int get64RandomBytes(char *buf);
void setSessionSignatureClientside( char* _sig );
void _rsSetAuthRequestGetChallenge( const char* _c );

static eirods::error check_proxy_user_privileges(
    rsComm_t *rsComm, 
    int proxyUserPriv )
{
    eirods::error result = SUCCESS();
    
    if (strcmp (rsComm->proxyUser.userName, rsComm->clientUser.userName) != 0) {

        /* remote privileged user can only do things on behalf of users from
         * the same zone */
        result = ASSERT_ERROR(proxyUserPriv >= LOCAL_PRIV_USER_AUTH ||
                              (proxyUserPriv >= REMOTE_PRIV_USER_AUTH &&
                               strcmp (rsComm->proxyUser.rodsZone,rsComm->clientUser.rodsZone) == 0),
                              SYS_PROXYUSER_NO_PRIV,
                              "Proxyuser: \"%s\" with %d no priv to auth clientUser: \"%s\".",
                              rsComm->proxyUser.userName, proxyUserPriv, rsComm->clientUser.userName);
    }

    return result;
}


extern "C" {

    // =-=-=-=-=-=-=-
    // NOTE:: this needs to become a property
    // Set requireServerAuth to 1 to fail authentications from
    // un-authenticated Servers (for example, if the LocalZoneSID 
    // is not set)
    const int requireServerAuth = 0;

    // =-=-=-=-=-=-=-
    // given the client connection and context string, set up the
    // native auth object with relevant informaiton: user, zone, etc
    eirods::error native_auth_client_start(
        eirods::auth_plugin_context& _ctx,
        rcComm_t*                    _comm, 
        const char*                  _context )
    {
        eirods::error result = SUCCESS();
        eirods::error ret;
        
        // =-=-=-=-=-=-=-
        // validate incoming parameters
        ret = _ctx.valid< eirods::native_auth_object >();
        if((result = ASSERT_PASS(ret, "Invalid plugin context.")).ok() ) {

            if((result = ASSERT_ERROR(_comm, SYS_INVALID_INPUT_PARAM, "Null rcConn_t pointer.")).ok()) {

                // =-=-=-=-=-=-=-
                // get the native auth object
                eirods::native_auth_object_ptr ptr = boost::dynamic_pointer_cast<eirods::native_auth_object >(_ctx.fco() );
                
                // =-=-=-=-=-=-=-
                // set the user name from the conn
                ptr->user_name( _comm->proxyUser.userName );
                
                // =-=-=-=-=-=-=-
                // set the zone name from the conn
                ptr->zone_name( _comm->proxyUser.rodsZone );
            }   
        }
        
        return result;
        
    } // native_auth_client_start
            
    // =-=-=-=-=-=-=-
    // establish context - take the auth request results and massage them
    // for the auth response call
    eirods::error native_auth_establish_context(
        eirods::auth_plugin_context& _ctx )
    {
        eirods::error result = SUCCESS();
        eirods::error ret;
        
        // =-=-=-=-=-=-=-
        // validate incoming parameters
        ret = _ctx.valid< eirods::native_auth_object >(); 
        if((result = ASSERT_PASS(ret, "Invalid plugin context.")).ok() ) {
               
            // =-=-=-=-=-=-=-
            // build a buffer for the challenge hash
            char md5_buf[ CHALLENGE_LEN + MAX_PASSWORD_LEN + 2 ];
            memset(md5_buf, 0, sizeof( md5_buf ) );
 
            // =-=-=-=-=-=-=-
            // get the native auth object
            eirods::native_auth_object_ptr ptr = boost::dynamic_pointer_cast<eirods::native_auth_object >(_ctx.fco() );
            
            // =-=-=-=-=-=-=-
            // copy the challenge into the md5 buffer
            strncpy(md5_buf, ptr->request_result().c_str(), CHALLENGE_LEN );

            // =-=-=-=-=-=-=-
            // Save a representation of some of the challenge string for use
            // as a session signiture 
            setSessionSignatureClientside( md5_buf );

            // =-=-=-=-=-=-=-
            // determine if a password challenge is needed,
            // are we anonymous or not?
            int need_password = 0;
            if( strncmp(ANONYMOUS_USER, ptr->user_name().c_str(), NAME_LEN ) == 0 ) {
                
                // =-=-=-=-=-=-=-
                // its an anonymous user - set the flag
                md5_buf[CHALLENGE_LEN+1]='\0';
                need_password = 0;

            } else {
                // =-=-=-=-=-=-=-
                // determine if a password is already in place
                need_password = obfGetPw( md5_buf+CHALLENGE_LEN );
            }

            // =-=-=-=-=-=-=-
            // prompt for a password if necessary
            if( 0 != need_password ) {
                int doStty=0;

#ifdef windows_platform
                if (ProcessType != CLIENT_PT)
                    return CODE(i);
#endif

                path p ("/bin/stty");
                if (exists(p)) {
                    system("/bin/stty -echo 2> /dev/null");
                    doStty=1;
                }
                printf("Enter your current iRODS password:");
                fgets(md5_buf+CHALLENGE_LEN, MAX_PASSWORD_LEN, stdin);
                if (doStty) {
                    system("/bin/stty echo 2> /dev/null");
                    printf("\n");
                }
            
                int md5_len = strlen( md5_buf );
                md5_buf[md5_len-1]='\0'; /* remove trailing \n */

            } // if need_password

            // =-=-=-=-=-=-=-
            // create a md5 hash of the challenge
            MD5_CTX context;
            MD5Init( &context );
            MD5Update(&context, (unsigned char*)md5_buf, CHALLENGE_LEN+MAX_PASSWORD_LEN );

            char digest[ RESPONSE_LEN+2 ];
            MD5Final( ( unsigned char* )digest, &context );

            // =-=-=-=-=-=-=-
            // make sure 'string' doesn'tend early -
            // scrub out any errant terminating chars
            // by incrementing their value by one
            for( int i = 0; i < RESPONSE_LEN; ++i ) {
                if( digest[ i ] == '\0' ) {
                    digest[ i ]++;  
                }                                      
            }

            // =-=-=-=-=-=-=-
            // cache the digest for the response
            ptr->digest( digest );
        }
        
        return result;

    } // native_auth_establish_context

    // =-=-=-=-=-=-=-
    // handle an client-side auth request call 
    eirods::error native_auth_client_request(
        eirods::auth_plugin_context& _ctx,
        rcComm_t*                    _comm )
    {
        eirods::error result = SUCCESS();
        eirods::error ret;
        
        // =-=-=-=-=-=-=-
        // validate incoming parameters
        ret = _ctx.valid< eirods::native_auth_object >();
        if((result = ASSERT_PASS(ret, "Invalid plugin context.")).ok() ) {
        
            // =-=-=-=-=-=-=-
            // make the call to our auth request
            authRequestOut_t* auth_request = 0;
            int status = rcAuthRequest(_comm, &auth_request );
            if(!(result = ASSERT_ERROR(status >= 0, status, "Call to rcAuthRequest failed.")).ok()) {
                free( auth_request->challenge );
                free( auth_request );
            } else {
                
                // =-=-=-=-=-=-=-
                // get the auth object
                eirods::native_auth_object_ptr ptr = boost::dynamic_pointer_cast<eirods::native_auth_object >( _ctx.fco() );
                
                // =-=-=-=-=-=-=-
                // cache the challenge
                if((result = ASSERT_ERROR(auth_request->challenge, 0, "Challenge attribute is blank.")).ok()) {
                    ptr->request_result( auth_request->challenge );
                }

                free( auth_request->challenge );
                free( auth_request );
            }
        }
        
        return result;
        
    } // native_auth_client_request

    // =-=-=-=-=-=-=-
    // handle an agent-side auth request call 
    eirods::error native_auth_agent_request(
        eirods::auth_plugin_context& _ctx,
        rsComm_t*                    _comm )
    {
        eirods::error result = SUCCESS();
        eirods::error ret;
        
        // =-=-=-=-=-=-=-
        // validate incoming parameters
        ret = _ctx.valid< eirods::native_auth_object >();
        if((result = ASSERT_PASS(ret, "Invalid plugin context.")).ok() ) {

            if((result = ASSERT_ERROR(_comm, SYS_INVALID_INPUT_PARAM, "Null comm pointer.")).ok()) {
        
                // =-=-=-=-=-=-=-
                // generate a random buffer and copy it to the challenge
                char buf[ CHALLENGE_LEN + 2 ];
                get64RandomBytes( buf );
        
                // =-=-=-=-=-=-=-
                // get the auth object
                eirods::native_auth_object_ptr ptr = boost::dynamic_pointer_cast<eirods::native_auth_object >( _ctx.fco() );
                
                // =-=-=-=-=-=-=-
                // cache the challenge
                ptr->request_result( buf );

                // =-=-=-=-=-=-=-
                // cache the challenge in the server for later usage
                _rsSetAuthRequestGetChallenge( buf );
            }
        }
        
        // =-=-=-=-=-=-=-
        // win!
        return SUCCESS(); 
        
    } // native_auth_agent_request

    // =-=-=-=-=-=-=-
    // handle a client-side auth request call 
    eirods::error native_auth_client_response(
        eirods::auth_plugin_context& _ctx,
        rcComm_t*                    _comm )
    {
        eirods::error result = SUCCESS();
        eirods::error ret;
        
        // =-=-=-=-=-=-=-
        // validate incoming parameters
        ret = _ctx.valid< eirods::native_auth_object >();
        if((result = ASSERT_PASS(ret, "Invalid plugin context.")).ok() ) {
            if((result = ASSERT_ERROR(_comm, SYS_INVALID_INPUT_PARAM, "Null rcComm_t pointer.")).ok()) { 
        
                // =-=-=-=-=-=-=-
                // get the auth object
                eirods::native_auth_object_ptr ptr = boost::dynamic_pointer_cast<eirods::native_auth_object >(_ctx.fco() );
                
                // =-=-=-=-=-=-=-
                // build the response string
                char response[ RESPONSE_LEN+2 ];
                strncpy(response, ptr->digest().c_str(), RESPONSE_LEN+2 );
               
                // =-=-=-=-=-=-=-
                // build the username#zonename string
                std::string user_name = ptr->user_name() + "#"              + ptr->zone_name();
                char username[ MAX_NAME_LEN ];
                strncpy(username, user_name.c_str(), MAX_NAME_LEN );

                authResponseInp_t auth_response;
                auth_response.response = response;
                auth_response.username = username;
                int status = rcAuthResponse(_comm, &auth_response );
                result = ASSERT_ERROR(status >= 0, status, "Call to rcAuthResponseFailed.");
            }
        }
        return result;           
    } // native_auth_client_response


    // TODO -This function really needs breaking into bite sized bits - harry
    
    // =-=-=-=-=-=-=-
    // handle an agent-side auth request call 
    eirods::error native_auth_agent_response(
        eirods::auth_plugin_context& _ctx,
        rsComm_t*                    _comm,
        authResponseInp_t*           _resp )
    {
        eirods::error result = SUCCESS();
        eirods::error ret;
        
        // =-=-=-=-=-=-=-
        // validate incoming parameters
        ret = _ctx.valid();
        if((result = ASSERT_PASS(ret, "Invalid plugin context.")).ok() ) {
            if((result = ASSERT_ERROR(_resp && _comm, SYS_INVALID_INPUT_PARAM, "Invalid response or comm pointers.")).ok()) {

                int status;
                char *bufp;
                authCheckInp_t authCheckInp;
                authCheckOut_t *authCheckOut = NULL;
                rodsServerHost_t *rodsServerHost;

                char digest[RESPONSE_LEN+2];
                char md5Buf[CHALLENGE_LEN+MAX_PASSWORD_LEN+2];
                char serverId[MAX_PASSWORD_LEN+2];
                MD5_CTX context;

                bufp = _rsAuthRequestGetChallenge();

                /* need to do NoLogin because it could get into inf loop for cross 
                 * zone auth */

                status = getAndConnRcatHostNoLogin ( _comm, MASTER_RCAT, 
                                                     _comm->proxyUser.rodsZone, &rodsServerHost);
                if((result = ASSERT_ERROR(status >= 0, status, "Connecting to rcat host failed.")).ok()) {

                    memset (&authCheckInp, 0, sizeof (authCheckInp)); 
                    authCheckInp.challenge = bufp;
                    authCheckInp.response = _resp->response;
                    authCheckInp.username = _resp->username;

                    if (rodsServerHost->localFlag == LOCAL_HOST) {
                        status = rsAuthCheck ( _comm, &authCheckInp, &authCheckOut);
                    } else {
                        status = rcAuthCheck (rodsServerHost->conn, &authCheckInp, &authCheckOut);
                        /* not likely we need this connection again */
                        rcDisconnect(rodsServerHost->conn);
                        rodsServerHost->conn = NULL;
                    }
                    if((result = ASSERT_ERROR(status >= 0 && authCheckOut != NULL, status, "rcAuthCheck failed." )).ok()) { // JMC cppcheck 

                        if (rodsServerHost->localFlag != LOCAL_HOST) {
                            if (authCheckOut->serverResponse == NULL) {
                                rodsLog(LOG_NOTICE, "Warning, cannot authenticate remote server, no serverResponse field");
                                result = ASSERT_ERROR(!requireServerAuth, REMOTE_SERVER_AUTH_NOT_PROVIDED, "Authentication disallowed. no serverResponse field.");
                            }

                            else {
                                char *cp;
                                int OK, len, i;
                                if (*authCheckOut->serverResponse == '\0') {
                                    rodsLog(LOG_NOTICE, "Warning, cannot authenticate remote server, serverResponse field is empty");
                                    result = ASSERT_ERROR(!requireServerAuth, REMOTE_SERVER_AUTH_EMPTY, "Authentication disallowed, empty serverResponse.");
                                }
                                else {
                                    char username2[NAME_LEN+2];
                                    char userZone[NAME_LEN+2];
                                    memset(md5Buf, 0, sizeof(md5Buf));
                                    strncpy(md5Buf, authCheckInp.challenge, CHALLENGE_LEN);
                                    parseUserName(_resp->username, username2, userZone);
                                    getZoneServerId(userZone, serverId);
                                    len = strlen(serverId);
                                    if (len <= 0) {
                                        rodsLog (LOG_NOTICE, "rsAuthResponse: Warning, cannot authenticate the remote server, no RemoteZoneSID defined in server.config", status);
                                        result = ASSERT_ERROR(!requireServerAuth, REMOTE_SERVER_SID_NOT_DEFINED, "Authentication disallowed, no RemoteZoneSID defined.");
                                    }
                                    else { 
                                        strncpy(md5Buf+CHALLENGE_LEN, serverId, len);
                                        MD5Init (&context);
                                        MD5Update (&context, (unsigned char*)md5Buf, CHALLENGE_LEN + MAX_PASSWORD_LEN);
                                        MD5Final ((unsigned char*)digest, &context);
                                        for (i=0;i<RESPONSE_LEN;i++) {
                                            if (digest[i]=='\0') digest[i]++;  /* make sure 'string' doesn't
                                                                                  end early*/
                                        }
                                        cp = authCheckOut->serverResponse;
                                        OK=1;
                                        for (i=0;i<RESPONSE_LEN;i++) {
                                            if (*cp++ != digest[i]) OK=0;
                                        }
                                        rodsLog(LOG_DEBUG, "serverResponse is OK/Not: %d", OK);
                                        result = ASSERT_ERROR(OK != 0, REMOTE_SERVER_AUTHENTICATION_FAILURE, "Authentication disallowed, server response incorrect.");
                                    }
                                }
                            }
                        }

                        /* Set the clientUser zone if it is null. */
                        if (result.ok() && strlen( _comm->clientUser.rodsZone)==0) {
                            zoneInfo_t *tmpZoneInfo;
                            status = getLocalZoneInfo (&tmpZoneInfo);
                            if((result = ASSERT_ERROR(status >= 0, status, "getLocalZoneInfo failed.")).ok()) {
                                strncpy( _comm->clientUser.rodsZone, tmpZoneInfo->zoneName, NAME_LEN);
                            }
                        }
                        

                        /* have to modify privLevel if the icat is a foreign icat because
                         * a local user in a foreign zone is not a local user in this zone
                         * and vice versa for a remote user
                         */
                        if (result.ok() && rodsServerHost->rcatEnabled == REMOTE_ICAT) {
                            
                            /* proxy is easy because rodsServerHost is based on proxy user */
                            if (authCheckOut->privLevel == LOCAL_PRIV_USER_AUTH) {
                                authCheckOut->privLevel = REMOTE_PRIV_USER_AUTH;
                            }
                            else if (authCheckOut->privLevel == LOCAL_USER_AUTH) {
                                authCheckOut->privLevel = REMOTE_USER_AUTH;
                            }
                            
                            /* adjust client user */
                            if (strcmp ( _comm->proxyUser.userName,  _comm->clientUser.userName) == 0) {
                                authCheckOut->clientPrivLevel = authCheckOut->privLevel;
                            } else {
                                zoneInfo_t *tmpZoneInfo;
                                status = getLocalZoneInfo (&tmpZoneInfo);
                                if((result = ASSERT_ERROR(status >= 0, status, "getLocalZoneInfo failed.")).ok()) {
                                    if (strcmp (tmpZoneInfo->zoneName,  _comm->clientUser.rodsZone) == 0) {
                                        /* client is from local zone */
                                        if (authCheckOut->clientPrivLevel == REMOTE_PRIV_USER_AUTH) {
                                            authCheckOut->clientPrivLevel = LOCAL_PRIV_USER_AUTH;
                                        } else if (authCheckOut->clientPrivLevel == REMOTE_USER_AUTH) {
                                            authCheckOut->clientPrivLevel = LOCAL_USER_AUTH;
                                        }
                                    } else {
                                        /* client is from remote zone */
                                        if (authCheckOut->clientPrivLevel == LOCAL_PRIV_USER_AUTH) {
                                            authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
                                        } else if (authCheckOut->clientPrivLevel == LOCAL_USER_AUTH) {
                                            authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
                                        }
                                    }
                                }
                            }
                        } else if (strcmp ( _comm->proxyUser.userName,  _comm->clientUser.userName) == 0) {
                            authCheckOut->clientPrivLevel = authCheckOut->privLevel;
                        }

                        if(result.ok()) {
                            ret = check_proxy_user_privileges( _comm, authCheckOut->privLevel );
                        
                            if ((result = ASSERT_PASS(ret, "Check proxy user priviledges failed.")).ok()) {
                                rodsLog(LOG_NOTICE,
                                        "rsAuthResponse set proxy authFlag to %d, client authFlag to %d, user:%s proxy:%s client:%s",
                                        authCheckOut->privLevel,
                                        authCheckOut->clientPrivLevel,
                                        authCheckInp.username,
                                        _comm->proxyUser.userName,
                                        _comm->clientUser.userName);
                                
                                if (strcmp ( _comm->proxyUser.userName,  _comm->clientUser.userName) != 0) {
                                    _comm->proxyUser.authInfo.authFlag = authCheckOut->privLevel;
                                    _comm->clientUser.authInfo.authFlag = authCheckOut->clientPrivLevel;
                                } else {        /* proxyUser and clientUser are the same */
                                    _comm->proxyUser.authInfo.authFlag =
                                        _comm->clientUser.authInfo.authFlag = authCheckOut->privLevel;
                                } 
                                
                            }
                        }
                    }
                }
                if(authCheckOut != NULL) {
                    if (authCheckOut->serverResponse != NULL) {
                        free(authCheckOut->serverResponse);
                    }

                    free (authCheckOut);
                }
            }
        }
        return result;
        
    } // native_auth_agent_response

    // =-=-=-=-=-=-=-
    // stub for ops that the native plug does 
    // not need to support 
    eirods::error native_auth_agent_verify(
        eirods::auth_plugin_context& _ctx,
        const char* _a,
        const char* _b,
        const char* _c ) {
        return SUCCESS();

    } // native_auth_agent_verify


    // =-=-=-=-=-=-=-
    // stub for ops that the native plug does 
    // not need to support 
    eirods::error native_auth_success_stub( 
        eirods::auth_plugin_context& _ctx ) {
        return SUCCESS();

    } // native_auth_success_stub

    // =-=-=-=-=-=-=-
    // derive a new native_auth auth plugin from
    // the auth plugin base class for handling
    // native authentication
    class native_auth_plugin : public eirods::auth {
    public:
        native_auth_plugin( 
            const std::string& _nm, 
            const std::string& _ctx ) :
            eirods::auth( 
                _nm, 
                _ctx ) {
        } // ctor

        ~native_auth_plugin() {
        }

    }; // class native_auth_plugin

    // =-=-=-=-=-=-=-
    // factory function to provide instance of the plugin
    eirods::auth* plugin_factory( 
        const std::string& _inst_name, 
        const std::string& _context ) {
        // =-=-=-=-=-=-=-
        // create an auth object
        native_auth_plugin* nat = new native_auth_plugin( 
            _inst_name,
            _context );
        if( !nat ) {
            rodsLog( LOG_ERROR, "plugin_factory - failed to alloc native_auth_plugin" );
            return 0;
        }
        
        // =-=-=-=-=-=-=-
        // fill in the operation table mapping call 
        // names to function names
        nat->add_operation( eirods::AUTH_CLIENT_START,         "native_auth_client_start" );
        nat->add_operation( eirods::AUTH_AGENT_START,          "native_auth_success_stub" );
        nat->add_operation( eirods::AUTH_ESTABLISH_CONTEXT,    "native_auth_establish_context" );
        nat->add_operation( eirods::AUTH_CLIENT_AUTH_REQUEST,  "native_auth_client_request" );
        nat->add_operation( eirods::AUTH_AGENT_AUTH_REQUEST,   "native_auth_agent_request" );
        nat->add_operation( eirods::AUTH_CLIENT_AUTH_RESPONSE, "native_auth_client_response" );
        nat->add_operation( eirods::AUTH_AGENT_AUTH_RESPONSE,  "native_auth_agent_response" );
        nat->add_operation( eirods::AUTH_AGENT_AUTH_VERIFY,    "native_auth_agent_verify" );

        eirods::auth* auth = dynamic_cast< eirods::auth* >( nat );
        if( !auth ) {
            rodsLog( LOG_ERROR, "failed to dynamic cast to eirods::auth*" );
        }

        return auth;

    } // plugin_factory

}; // extern "C"