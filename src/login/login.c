// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/core.h"
#include "../common/db.h"
#include "../common/malloc.h"
#include "../common/md5calc.h"
#include "../common/showmsg.h"
#include "../common/socket.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "../common/version.h"
#include "account.h"
#include "login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// permanent imports
void login_log(uint32 ip, const char* username, int rcode, const char* message);

#ifdef TXT_ONLY
	bool login_config_read_txt(const char* w1, const char* w2);
#else
	bool login_config_read_sql(const char* w1, const char* w2);
	bool inter_config_read_sql(const char* w1, const char* w2);
#endif

// temporary external imports
extern struct gm_account* gm_account_db;
int read_gm_account(void);
void send_GM_accounts(int fd);

#ifdef TXT_ONLY
	int check_GM_file(int tid, unsigned int tick, int id, int data);
	void display_conf_warnings(void);
	int isGM(int account_id);
	extern char GM_account_filename[1024];
	extern int gm_account_filename_check_timer;
#else
	void mmo_db_init();
	void mmo_db_close();
	void login_dynamic_ipban(const char* userid, int ip);
	int ip_ban_flush(int tid, unsigned int tick, int id, int data);
#endif


struct Login_Config login_config;

int login_fd; // login server socket
#define MAX_SERVERS 30
struct mmo_char_server server[MAX_SERVERS]; // char server data

#define sex_num2str(num) ( (num ==  0  ) ? 'F' : (num ==  1  ) ? 'M' : 'S' )
#define sex_str2num(str) ( (str == 'F' ) ?  0  : (str == 'M' ) ?  1  :  2  )

// account database
AccountDB* accounts;

//Account registration flood protection [Kevin]
int allowed_regs = 1;
int time_allowed = 10; //in seconds

// Advanced subnet check [LuzZza]
struct s_subnet {
	uint32 mask;
	uint32 char_ip;
	uint32 map_ip;
} subnet[16];

int subnet_count = 0;


//-----------------------------------------------------
// Auth database
//-----------------------------------------------------
#define AUTH_TIMEOUT 30000

/*static*/ DBMap* auth_db; // int account_id -> struct auth_node*

//-----------------------------------------------------
// Online User Database [Wizputer]
//-----------------------------------------------------

/*static*/ DBMap* online_db; // int account_id -> struct online_login_data*
/*static*/ int waiting_disconnect_timer(int tid, unsigned int tick, int id, int data);

/*static*/ void* create_online_user(DBKey key, va_list args)
{
	struct online_login_data* p;
	CREATE(p, struct online_login_data, 1);
	p->account_id = key.i;
	p->char_server = -1;
	p->waiting_disconnect = -1;
	return p;
}

struct online_login_data* add_online_user(int char_server, int account_id)
{
	struct online_login_data* p;
	if( !login_config.online_check )
		return NULL;
	p = (struct online_login_data*)idb_ensure(online_db, account_id, create_online_user);
	p->char_server = char_server;
	if( p->waiting_disconnect != -1 )
	{
		delete_timer(p->waiting_disconnect, waiting_disconnect_timer);
		p->waiting_disconnect = -1;
	}
	return p;
}

void remove_online_user(int account_id)
{
	struct online_login_data* p;
	if( !login_config.online_check )
		return;
	p = (struct online_login_data*)idb_get(online_db, account_id);
	if( p == NULL )
		return;
	if( p->waiting_disconnect != -1 )
		delete_timer(p->waiting_disconnect, waiting_disconnect_timer);

	idb_remove(online_db, account_id);
}

/*static*/ int waiting_disconnect_timer(int tid, unsigned int tick, int id, int data)
{
	struct online_login_data* p = (struct online_login_data*)idb_get(online_db, id);
	if( p != NULL && p->waiting_disconnect == tid && p->account_id == id )
	{
		p->waiting_disconnect = -1;
		remove_online_user(id);
		idb_remove(auth_db, id);
	}
	return 0;
}

/*static*/ int online_db_setoffline(DBKey key, void* data, va_list ap)
{
	struct online_login_data* p = (struct online_login_data*)data;
	int server = va_arg(ap, int);
	if( server == -1 )
	{
		p->char_server = -1;
		if( p->waiting_disconnect != -1 )
		{
			delete_timer(p->waiting_disconnect, waiting_disconnect_timer);
			p->waiting_disconnect = -1;
		}
	}
	else if( p->char_server == server )
		p->char_server = -2; //Char server disconnected.
	return 0;
}

static int online_data_cleanup_sub(DBKey key, void *data, va_list ap)
{
	struct online_login_data *character= (struct online_login_data*)data;
	if (character->char_server == -2) //Unknown server.. set them offline
		remove_online_user(character->account_id);
	return 0;
}

static int online_data_cleanup(int tid, unsigned int tick, int id, int data)
{
	online_db->foreach(online_db, online_data_cleanup_sub);
	return 0;
} 


//--------------------------------------------------------------------
// Packet send to all char-servers, except one (wos: without our self)
//--------------------------------------------------------------------
int charif_sendallwos(int sfd, uint8* buf, size_t len)
{
	int i, c;

	for( i = 0, c = 0; i < MAX_SERVERS; ++i )
	{
		int fd = server[i].fd;
		if( session_isValid(fd) && fd != sfd )
		{
			WFIFOHEAD(fd,len);
			memcpy(WFIFOP(fd,0), buf, len);
			WFIFOSET(fd,len);
			++c;
		}
	}

	return c;
}


//-----------------------------------------------------
// periodic ip address synchronization
//-----------------------------------------------------
static int sync_ip_addresses(int tid, unsigned int tick, int id, int data)
{
	uint8 buf[2];
	ShowInfo("IP Sync in progress...\n");
	WBUFW(buf,0) = 0x2735;
	charif_sendallwos(-1, buf, 2);
	return 0;
}


//-----------------------------------------------------
// encrypted/unencrypted password check
//-----------------------------------------------------
bool check_encrypted(const char* str1, const char* str2, const char* passwd)
{
	char md5str[64], md5bin[32];

	snprintf(md5str, sizeof(md5str), "%s%s", str1, str2);
	md5str[sizeof(md5str)-1] = '\0';
	MD5_String2binary(md5str, md5bin);

	return (0==memcmp(passwd, md5bin, 16));
}

bool check_password(struct login_session_data* sd, int passwdenc, const char* passwd, const char* refpass)
{	
	if(passwdenc == 0)
	{
		return (0==strcmp(passwd, refpass));
	}
	else if(sd != NULL)
	{
		// password mode set to 1 -> (md5key, refpass) enable with <passwordencrypt></passwordencrypt>
		// password mode set to 2 -> (refpass, md5key) enable with <passwordencrypt2></passwordencrypt2>
		
		return ((passwdenc&0x01) && check_encrypted(sd->md5key, refpass, passwd)) ||
		       ((passwdenc&0x02) && check_encrypted(refpass, sd->md5key, passwd));
	}
	return false;
}


//--------------------------------------------
// Test to know if an IP come from LAN or WAN.
//--------------------------------------------
int lan_subnetcheck(uint32 ip)
{
	int i;
	ARR_FIND( 0, subnet_count, i, (subnet[i].char_ip & subnet[i].mask) == (ip & subnet[i].mask) );
	return ( i < subnet_count ) ? subnet[i].char_ip : 0;
}

//----------------------------------
// Reading Lan Support configuration
//----------------------------------
int login_lan_config_read(const char *lancfgName)
{
	FILE *fp;
	int line_num = 0;
	char line[1024], w1[64], w2[64], w3[64], w4[64];

	if((fp = fopen(lancfgName, "r")) == NULL) {
		ShowWarning("LAN Support configuration file is not found: %s\n", lancfgName);
		return 1;
	}

	ShowInfo("Reading the configuration file %s...\n", lancfgName);

	while(fgets(line, sizeof(line), fp))
	{
		line_num++;
		if ((line[0] == '/' && line[1] == '/') || line[0] == '\n' || line[1] == '\n')
			continue;

		if(sscanf(line,"%[^:]: %[^:]:%[^:]:%[^\r\n]", w1, w2, w3, w4) != 4)
		{
			ShowWarning("Error syntax of configuration file %s in line %d.\n", lancfgName, line_num);
			continue;
		}

		if( strcmpi(w1, "subnet") == 0 )
		{
			subnet[subnet_count].mask = str2ip(w2);
			subnet[subnet_count].char_ip = str2ip(w3);
			subnet[subnet_count].map_ip = str2ip(w4);

			if( (subnet[subnet_count].char_ip & subnet[subnet_count].mask) != (subnet[subnet_count].map_ip & subnet[subnet_count].mask) )
			{
				ShowError("%s: Configuration Error: The char server (%s) and map server (%s) belong to different subnetworks!\n", lancfgName, w3, w4);
				continue;
			}

			subnet_count++;
		}
	}

	ShowStatus("Read information about %d subnetworks.\n", subnet_count);

	fclose(fp);
	return 0;
}


//-----------------------
// Console Command Parser [Wizputer]
//-----------------------
int parse_console(char* buf)
{
	char command[256];

	memset(command, 0, sizeof(command));

	sscanf(buf, "%[^\n]", command);

	ShowInfo("Console command :%s", command);

	if( strcmpi("shutdown", command) == 0 ||
	    strcmpi("exit", command) == 0 ||
	    strcmpi("quit", command) == 0 ||
	    strcmpi("end", command) == 0 )
		runflag = 0;
	else
	if( strcmpi("alive", command) == 0 ||
	    strcmpi("status", command) == 0 )
		ShowInfo(CL_CYAN"Console: "CL_BOLD"I'm Alive."CL_RESET"\n");
	else
	if( strcmpi("help", command) == 0 ) {
		ShowInfo(CL_BOLD"Help of commands:"CL_RESET"\n");
		ShowInfo("  To shutdown the server:\n");
		ShowInfo("  'shutdown|exit|quit|end'\n");
		ShowInfo("  To know if server is alive:\n");
		ShowInfo("  'alive|status'\n");
	}

	return 0;
}


//--------------------------------
// Packet parsing for char-servers
//--------------------------------
int parse_fromchar(int fd)
{
	int j, id;
	uint32 ipl;
	char ip[16];

	ARR_FIND( 0, MAX_SERVERS, id, server[id].fd == fd );
	if( id == MAX_SERVERS )
	{// not a char server
		set_eof(fd);
		do_close(fd);
		return 0;
	}

	if( session[fd]->flag.eof )
	{
		ShowStatus("Char-server '%s' has disconnected.\n", server[id].name);
		online_db->foreach(online_db, online_db_setoffline, id); //Set all chars from this char server to offline.
		memset(&server[id], 0, sizeof(struct mmo_char_server));
		server[id].fd = -1;
		do_close(fd);
		return 0;
	}

	ipl = server[id].ip;
	ip2str(ipl, ip);

	while( RFIFOREST(fd) >= 2 )
	{
		uint16 command = RFIFOW(fd,0);

		switch( command )
		{

		case 0x2709: // request from map-server via char-server to reload GM accounts
			RFIFOSKIP(fd,2);
			ShowStatus("Char-server '%s': Request to re-load GM database (ip: %s).\n", server[id].name, ip);
			login_log(ipl, server[id].name, 0, "GM reload request");
			read_gm_account();
			// send GM accounts to all char-servers
			send_GM_accounts(-1);
		break;

		case 0x2712: // request from char-server to authenticate an account
			if( RFIFOREST(fd) < 19 )
				return 0;
		{
			struct auth_node* node;

			int account_id = RFIFOL(fd,2);
			int login_id1 = RFIFOL(fd,6);
			int login_id2 = RFIFOL(fd,10);
			char sex = sex_num2str(RFIFOB(fd,14));
			uint32 ip_ = ntohl(RFIFOL(fd,15));
			RFIFOSKIP(fd,19);

			node = (struct auth_node*)idb_get(auth_db, account_id);
			if( node != NULL &&
			    node->account_id == account_id &&
				node->login_id1  == login_id1 &&
				node->login_id2  == login_id2 &&
				node->sex        == sex &&
				node->ip         == ip_ )
			{// found
				struct mmo_account acc;
				time_t expiration_time = 0;
				const char* email = "";

				//ShowStatus("Char-server '%s': authentication of the account %d accepted (ip: %s).\n", server[id].name, account_id, ip);

				// each auth entry can only be used once
				idb_remove(auth_db, account_id);

				// retrieve email and account expiration time
				if( accounts->load_num(accounts, &acc, account_id) )
				{
					email = acc.email;
					expiration_time = acc.expiration_time;
				}

				// send ack
				WFIFOHEAD(fd,59);
				WFIFOW(fd,0) = 0x2713;
				WFIFOL(fd,2) = account_id;
				WFIFOL(fd,6) = login_id1;
				WFIFOL(fd,10) = login_id2;
				WFIFOB(fd,14) = 0;
				safestrncpy((char*)WFIFOP(fd,15), email, 40);
				WFIFOL(fd,55) = (uint32)expiration_time;
				WFIFOSET(fd,59);
			}
			else
			{// authentication not found
				ShowStatus("Char-server '%s': authentication of the account %d REFUSED (ip: %s).\n", server[id].name, account_id, ip);
				WFIFOHEAD(fd,59);
				WFIFOW(fd,0) = 0x2713;
				WFIFOL(fd,2) = account_id;
				WFIFOL(fd,6) = login_id1;
				WFIFOL(fd,10) = login_id2;
				WFIFOB(fd,14) = 1;
				// It is unnecessary to send email
				// It is unnecessary to send validity date of the account
				WFIFOSET(fd,59);
			}
		}
		break;

		case 0x2714:
			if( RFIFOREST(fd) < 6 )
				return 0;
		{
			int users = RFIFOL(fd,2);
			RFIFOSKIP(fd,6);

			// how many users on world? (update)
			if( server[id].users != users )
			{
				ShowStatus("set users %s : %d\n", server[id].name, users);

				server[id].users = users;
			}
		}
		break;

		case 0x2715: // request from char server to change e-email from default "a@a.com"
			if (RFIFOREST(fd) < 46)
				return 0;
		{
			struct mmo_account acc;
			char email[40];

			int account_id = RFIFOL(fd,2);
			safestrncpy(email, (char*)RFIFOP(fd,6), 40); remove_control_chars(email);
			RFIFOSKIP(fd,46);

			if( e_mail_check(email) == 0 )
				ShowNotice("Char-server '%s': Attempt to create an e-mail on an account with a default e-mail REFUSED - e-mail is invalid (account: %d, ip: %s)\n", server[id].name, account_id, ip);
			else
			if( !accounts->load_num(accounts, &acc, account_id) || strcmp(acc.email, "a@a.com") == 0 || acc.email[0] == '\0' )
				ShowNotice("Char-server '%s': Attempt to create an e-mail on an account with a default e-mail REFUSED - account doesn't exist or e-mail of account isn't default e-mail (account: %d, ip: %s).\n", server[id].name, account_id, ip);
			else {
				memcpy(acc.email, email, 40);
				ShowNotice("Char-server '%s': Create an e-mail on an account with a default e-mail (account: %d, new e-mail: %s, ip: %s).\n", server[id].name, account_id, email, ip);
				// Save
				accounts->save(accounts, &acc);
			}
		}
		break;

		case 0x2716: // received an e-mail/limited time request, because a player comes back from a map-server to the char-server
			if( RFIFOREST(fd) < 6 )
				return 0;
		{
			struct mmo_account acc;
			time_t expiration_time = 0;
			char email[40] = "";

			int account_id = RFIFOL(fd,2);
			RFIFOSKIP(fd,6);

			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowNotice("Char-server '%s': e-mail of the account %d NOT found (ip: %s).\n", server[id].name, account_id, ip);
			else
			{
				safestrncpy(email, acc.email, sizeof(email));
				expiration_time = acc.expiration_time;
			}

			WFIFOHEAD(fd,50);
			WFIFOW(fd,0) = 0x2717;
			WFIFOL(fd,2) = account_id;
			safestrncpy((char*)WFIFOP(fd,6), email, 40);
			WFIFOL(fd,46) = (uint32)expiration_time;
			WFIFOSET(fd,50);
		}
		break;

		case 0x2719: // ping request from charserver
			if( RFIFOREST(fd) < 2 )
				return 0;
			RFIFOSKIP(fd,2);

			WFIFOHEAD(fd,2);
			WFIFOW(fd,0) = 0x2718;
			WFIFOSET(fd,2);
		break;

		// Map server send information to change an email of an account via char-server
		case 0x2722:	// 0x2722 <account_id>.L <actual_e-mail>.40B <new_e-mail>.40B
			if (RFIFOREST(fd) < 86)
				return 0;
		{
			struct mmo_account acc;
			char actual_email[40];
			char new_email[40];

			int account_id = RFIFOL(fd,2);
			safestrncpy(actual_email, (char*)RFIFOP(fd,6), 40);
			safestrncpy(new_email, (char*)RFIFOP(fd,46), 40);
			RFIFOSKIP(fd, 86);

			if( e_mail_check(actual_email) == 0 )
				ShowNotice("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command), but actual email is invalid (account: %d, ip: %s)\n", server[id].name, account_id, ip);
			else
			if( e_mail_check(new_email) == 0 )
				ShowNotice("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command) with a invalid new e-mail (account: %d, ip: %s)\n", server[id].name, account_id, ip);
			else
			if( strcmpi(new_email, "a@a.com") == 0 )
				ShowNotice("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command) with a default e-mail (account: %d, ip: %s)\n", server[id].name, account_id, ip);
			else
			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowNotice("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command), but account doesn't exist (account: %d, ip: %s).\n", server[id].name, account_id, ip);
			else
			if( strcmpi(acc.email, actual_email) != 0 )
				ShowNotice("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command), but actual e-mail is incorrect (account: %d (%s), actual e-mail: %s, proposed e-mail: %s, ip: %s).\n", server[id].name, account_id, acc.userid, acc.email, actual_email, ip);
			else {
				safestrncpy(acc.email, new_email, 40);
				ShowNotice("Char-server '%s': Modify an e-mail on an account (@email GM command) (account: %d (%s), new e-mail: %s, ip: %s).\n", server[id].name, account_id, acc.userid, new_email, ip);
				// Save
				accounts->save(accounts, &acc);
			}
		}
		break;

		case 0x2724: // Receiving an account state update request from a map-server (relayed via char-server)
			if (RFIFOREST(fd) < 10)
				return 0;
		{
			struct mmo_account acc;

			int account_id = RFIFOL(fd,2);
			int state = RFIFOL(fd,6);
			RFIFOSKIP(fd,10);

			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowNotice("Char-server '%s': Error of Status change (account: %d not found, suggested status %d, ip: %s).\n", server[id].name, account_id, state, ip);
			else
			if( acc.state == state )
				ShowNotice("Char-server '%s':  Error of Status change - actual status is already the good status (account: %d, status %d, ip: %s).\n", server[id].name, account_id, state, ip);
			else {
				ShowNotice("Char-server '%s': Status change (account: %d, new status %d, ip: %s).\n", server[id].name, account_id, state, ip);

				acc.state = state;
				// Save
				accounts->save(accounts, &acc);

				// notify other servers
				if (state != 0) {
					uint8 buf[11];
					WBUFW(buf,0) = 0x2731;
					WBUFL(buf,2) = account_id;
					WBUFB(buf,6) = 0; // 0: change of state, 1: ban
					WBUFL(buf,7) = state; // status or final date of a banishment
					charif_sendallwos(-1, buf, 11);
				}
			}
		}
		break;

		case 0x2725: // Receiving of map-server via char-server a ban request
			if (RFIFOREST(fd) < 18)
				return 0;
		{
			struct mmo_account acc;

			int account_id = RFIFOL(fd,2);
			int year = (short)RFIFOW(fd,6);
			int month = (short)RFIFOW(fd,8);
			int mday = (short)RFIFOW(fd,10);
			int hour = (short)RFIFOW(fd,12);
			int min = (short)RFIFOW(fd,14);
			int sec = (short)RFIFOW(fd,16);
			RFIFOSKIP(fd,18);

			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowNotice("Char-server '%s': Error of ban request (account: %d not found, ip: %s).\n", server[id].name, account_id, ip);
			else
			{
				time_t timestamp;
				struct tm *tmtime;
				if (acc.unban_time == 0 || acc.unban_time < time(NULL))
					timestamp = time(NULL); // new ban
				else
					timestamp = acc.unban_time; // add to existing ban
				tmtime = localtime(&timestamp);
				tmtime->tm_year = tmtime->tm_year + year;
				tmtime->tm_mon  = tmtime->tm_mon + month;
				tmtime->tm_mday = tmtime->tm_mday + mday;
				tmtime->tm_hour = tmtime->tm_hour + hour;
				tmtime->tm_min  = tmtime->tm_min + min;
				tmtime->tm_sec  = tmtime->tm_sec + sec;
				timestamp = mktime(tmtime);
				if (timestamp == -1)
					ShowNotice("Char-server '%s': Error of ban request (account: %d, invalid date, ip: %s).\n", server[id].name, account_id, ip);
				else
				if( timestamp <= time(NULL) || timestamp == 0 )
					ShowNotice("Char-server '%s': Error of ban request (account: %d, new date unbans the account, ip: %s).\n", server[id].name, account_id, ip);
				else
				{
					uint8 buf[11];
					char tmpstr[2048];
					strftime(tmpstr, 24, login_config.date_format, localtime(&timestamp));
					ShowNotice("Char-server '%s': Ban request (account: %d, new final date of banishment: %d (%s), ip: %s).\n", server[id].name, account_id, timestamp, tmpstr, ip);

					acc.unban_time = timestamp;
					// Save
					accounts->save(accounts, &acc);

					WBUFW(buf,0) = 0x2731;
					WBUFL(buf,2) = account_id;
					WBUFB(buf,6) = 1; // 0: change of status, 1: ban
					WBUFL(buf,7) = (uint32)timestamp; // status or final date of a banishment
					charif_sendallwos(-1, buf, 11);
				}
			}
		}
		break;

		case 0x2727: // Change of sex (sex is reversed)
			if( RFIFOREST(fd) < 6 )
				return 0;
		{
			struct mmo_account acc;

			int account_id = RFIFOL(fd,2);
			RFIFOSKIP(fd,6);

			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowNotice("Char-server '%s': Error of sex change (account: %d not found, ip: %s).\n", server[id].name, account_id, ip);
			else
			if( acc.sex == 'S' )
				ShowNotice("Char-server '%s': Error of sex change - account to change is a Server account (account: %d, ip: %s).\n", server[id].name, account_id, ip);
			else
			{
				unsigned char buf[7];
				char sex = ( acc.sex == 'M' ) ? 'F' : 'M'; //Change gender

				ShowNotice("Char-server '%s': Sex change (account: %d, new sex %c, ip: %s).\n", server[id].name, account_id, sex, ip);

				acc.sex = sex;
				// Save
				accounts->save(accounts, &acc);

				// announce to other servers
				WBUFW(buf,0) = 0x2723;
				WBUFL(buf,2) = account_id;
				WBUFB(buf,6) = sex_str2num(sex);
				charif_sendallwos(-1, buf, 7);
			}
		}
		break;

		case 0x2728:	// We receive account_reg2 from a char-server, and we send them to other map-servers.
			if( RFIFOREST(fd) < 4 || RFIFOREST(fd) < RFIFOW(fd,2) )
				return 0;
		{
			struct mmo_account acc;

			int account_id = RFIFOL(fd,4);

			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowStatus("Char-server '%s': receiving (from the char-server) of account_reg2 (account: %d not found, ip: %s).\n", server[id].name, account_id, ip);
			else
			{
				int len;
				int p;
				ShowNotice("char-server '%s': receiving (from the char-server) of account_reg2 (account: %d, ip: %s).\n", server[id].name, account_id, ip);
				for( j = 0, p = 13; j < ACCOUNT_REG2_NUM && p < RFIFOW(fd,2); ++j )
				{
					sscanf((char*)RFIFOP(fd,p), "%31c%n", acc.account_reg2[j].str, &len);
					acc.account_reg2[j].str[len]='\0';
					p +=len+1; //+1 to skip the '\0' between strings.
					sscanf((char*)RFIFOP(fd,p), "%255c%n", acc.account_reg2[j].value, &len);
					acc.account_reg2[j].value[len]='\0';
					p +=len+1;
					remove_control_chars(acc.account_reg2[j].str);
					remove_control_chars(acc.account_reg2[j].value);
				}
				acc.account_reg2_num = j;

				// Save
				accounts->save(accounts, &acc);

				// Sending information towards the other char-servers.
				RFIFOW(fd,0) = 0x2729;// reusing read buffer
				charif_sendallwos(fd, RFIFOP(fd,0), RFIFOW(fd,2));
				RFIFOSKIP(fd,RFIFOW(fd,2));
			}
		}
		break;

		case 0x272a:	// Receiving of map-server via char-server an unban request
			if( RFIFOREST(fd) < 6 )
				return 0;
		{
			struct mmo_account acc;

			int account_id = RFIFOL(fd,2);
			RFIFOSKIP(fd,6);

			if( !accounts->load_num(accounts, &acc, account_id) )
				ShowNotice("Char-server '%s': Error of UnBan request (account: %d not found, ip: %s).\n", server[id].name, account_id, ip);
			else
			if( acc.unban_time == 0 )
				ShowNotice("Char-server '%s': Error of UnBan request (account: %d, no change for unban date, ip: %s).\n", server[id].name, account_id, ip);
			else
			{
				ShowNotice("Char-server '%s': UnBan request (account: %d, ip: %s).\n", server[id].name, account_id, ip);
				acc.unban_time = 0;
				accounts->save(accounts, &acc);
			}
		}
		break;

		case 0x272b:    // Set account_id to online [Wizputer]
			if( RFIFOREST(fd) < 6 )
				return 0;
			add_online_user(id, RFIFOL(fd,2));
			RFIFOSKIP(fd,6);
		break;

		case 0x272c:   // Set account_id to offline [Wizputer]
			if( RFIFOREST(fd) < 6 )
				return 0;
			remove_online_user(RFIFOL(fd,2));
			RFIFOSKIP(fd,6);
		break;

		case 0x272d:	// Receive list of all online accounts. [Skotlex]
			if (RFIFOREST(fd) < 4 || RFIFOREST(fd) < RFIFOW(fd,2))
				return 0;
			if( login_config.online_check )
			{
				struct online_login_data *p;
				int aid;
				uint32 i, users;
				online_db->foreach(online_db, online_db_setoffline, id); //Set all chars from this char-server offline first
				users = RFIFOW(fd,4);
				for (i = 0; i < users; i++) {
					aid = RFIFOL(fd,6+i*4);
					p = (struct online_login_data*)idb_ensure(online_db, aid, create_online_user);
					p->char_server = id;
					if (p->waiting_disconnect != -1)
					{
						delete_timer(p->waiting_disconnect, waiting_disconnect_timer);
						p->waiting_disconnect = -1;
					}
				}
			}
			RFIFOSKIP(fd,RFIFOW(fd,2));
		break;

		case 0x272e: //Request account_reg2 for a character.
			if (RFIFOREST(fd) < 10)
				return 0;
		{
			struct mmo_account acc;
			size_t off;

			int account_id = RFIFOL(fd,2);
			int char_id = RFIFOL(fd,6);
			RFIFOSKIP(fd,10);

			WFIFOHEAD(fd,10000);
			WFIFOW(fd,0) = 0x2729;
			WFIFOL(fd,4) = account_id;
			WFIFOL(fd,8) = char_id;
			WFIFOB(fd,12) = 1; //Type 1 for Account2 registry

			off = 13;
			if( accounts->load_num(accounts, &acc, account_id) )
			{
				for( j = 0; j < acc.account_reg2_num && off < 9000; j++ )
				{
					if( acc.account_reg2[j].str[0] != '\0' )
					{
						off += sprintf((char*)WFIFOP(fd,off), "%s", acc.account_reg2[j].str)+1; //We add 1 to consider the '\0' in place.
						off += sprintf((char*)WFIFOP(fd,off), "%s", acc.account_reg2[j].value)+1;
					}
				}

				if( off >= 9000 )
					ShowWarning("Too many account2 registries for AID %d. Some registries were not sent.\n", account_id);
			}

			WFIFOW(fd,2) = (uint16)off;
			WFIFOSET(fd,WFIFOW(fd,2));
		}
		break;

		case 0x2736: // WAN IP update from char-server
			if( RFIFOREST(fd) < 6 )
				return 0;
			server[id].ip = ntohl(RFIFOL(fd,2));
			ShowInfo("Updated IP of Server #%d to %d.%d.%d.%d.\n",id, CONVIP(server[id].ip));
			RFIFOSKIP(fd,6);
		break;

		case 0x2737: //Request to set all offline.
			ShowInfo("Setting accounts from char-server %d offline.\n", id);
			online_db->foreach(online_db, online_db_setoffline, id);
			RFIFOSKIP(fd,2);
		break;

		default:
			ShowError("parse_fromchar: Unknown packet 0x%x from a char-server! Disconnecting!\n", command);
			set_eof(fd);
			return 0;
		} // switch
	} // while

	RFIFOSKIP(fd,RFIFOREST(fd));
	return 0;
}


//-------------------------------------
// Make new account
//-------------------------------------
int mmo_auth_new(struct mmo_account* account)
{
	static int num_regs = 0; // registration counter
	static unsigned int new_reg_tick = 0;
	unsigned int tick = gettick();
	struct mmo_account tmpacc;

	//Account Registration Flood Protection by [Kevin]
	if( new_reg_tick == 0 )
		new_reg_tick = gettick();
	if( DIFF_TICK(tick, new_reg_tick) < 0 && num_regs >= allowed_regs )
	{
		ShowNotice("Account registration denied (registration limit exceeded)\n");
		return 3;
	}

	// check if the account doesn't exist already
	if( accounts->load_str(accounts, &tmpacc, account->userid) )
	{
		ShowNotice("Attempt of creation of an already existant account (account: %s_%c, pass: %s, received pass: %s)\n", account->userid, account->sex, tmpacc.pass, account->pass);
		return 1; // 1 = Incorrect Password
	}

	account->account_id = 0; // assigned by account db
	safestrncpy(account->lastlogin, "-", sizeof(account->lastlogin));
	account->logincount = 0;
	account->state = 0;
	safestrncpy(account->email, "a@a.com", sizeof(account->email));
	account->unban_time = 0;
	account->expiration_time = ( login_config.start_limited_time != -1 ) ? time(NULL) + login_config.start_limited_time : 0;
	strncpy(account->last_ip, "-", 16);
	strncpy(account->memo, "-", 255);
	account->account_reg2_num = 0;

	accounts->create(accounts, account);
	ShowNotice("Account creation (account %s, pass: %s, sex: %c)\n", account->userid, account->pass, account->sex);

	if( DIFF_TICK(tick, new_reg_tick) > 0 )
	{// Update the registration check.
		num_regs = 0;
		new_reg_tick = tick + time_allowed*1000;
	}
	++num_regs;

	return 0;
}

//-----------------------------------------------------
// Check/authentication of a connection
//-----------------------------------------------------
int mmo_auth(struct login_session_data* sd)
{
	time_t raw_time;
	char tmpstr[256];
	int len;
	char user_password[32+1]; // reserve for md5-ed pw
	struct mmo_account acc;

	char ip[16];
	ip2str(session[sd->fd]->client_addr, ip);

	// DNS Blacklist check
	if( login_config.use_dnsbl )
	{
		char r_ip[16];
		char ip_dnsbl[256];
		char* dnsbl_serv;
		bool matched = false;
		uint8* sin_addr = (uint8*)&session[sd->fd]->client_addr;

		sprintf(r_ip, "%u.%u.%u.%u", sin_addr[0], sin_addr[1], sin_addr[2], sin_addr[3]);

		for( dnsbl_serv = strtok(login_config.dnsbl_servs,","); !matched && dnsbl_serv != NULL; dnsbl_serv = strtok(NULL,",") )
		{
			sprintf(ip_dnsbl, "%s.%s", r_ip, dnsbl_serv);
			if( host2ip(ip_dnsbl) )
				matched = true;
		}

		if( matched )
		{
			ShowInfo("DNSBL: (%s) Blacklisted. User Kicked.\n", r_ip);
			return 3;
		}
	}

	//Client Version check
	if( login_config.check_client_version && sd->version != login_config.client_version_to_connect )
		return 5;

	len = strnlen(sd->userid, NAME_LENGTH);

	// Account creation with _M/_F
	if( login_config.new_account_flag )
	{
		if( len > 2 && strnlen(sd->passwd, NAME_LENGTH) > 0 && // valid user and password lengths
			sd->passwdenc == 0 && // unencoded password
			sd->userid[len-2] == '_' && memchr("FfMm", sd->userid[len-1], 4) ) // _M/_F suffix
		{
			struct mmo_account newacc;
			int result;

			// remove the _M/_F suffix
			len -= 2;
			sd->userid[len] = '\0';

			memset(&newacc, '\0', sizeof(acc));
			safestrncpy(newacc.userid, sd->userid, NAME_LENGTH);
			if( login_config.use_md5_passwds )
				MD5_String(sd->passwd, newacc.pass);
			else
				safestrncpy(newacc.pass, sd->passwd, NAME_LENGTH);
			newacc.sex = TOUPPER(sd->userid[len+1]);

			result = mmo_auth_new(&newacc);
			if( result )
				return result;// Failed to make account. [Skotlex].
		}
	}
	
	//TODO: case-sensitive/insensitive option
	if( !accounts->load_str(accounts, &acc, sd->userid) )
	{
		ShowNotice("Unknown account (account: %s, received pass: %s, ip: %s)\n", sd->userid, sd->passwd, ip);
		return 0; // 0 = Unregistered ID
	}

	if( login_config.use_md5_passwds )
		MD5_String(sd->passwd, user_password);
	else
		safestrncpy(user_password, sd->passwd, NAME_LENGTH);

	if( !check_password(sd, sd->passwdenc, user_password, acc.pass) )
	{
		ShowNotice("Invalid password (account: '%s', pass: '%s', received pass: '%s', ip: %s)\n", sd->userid, acc.pass, (sd->passwdenc) ? "[MD5]" : sd->passwd, ip);
		return 1; // 1 = Incorrect Password
	}

	if( acc.expiration_time != 0 && acc.expiration_time < time(NULL) )
	{
		ShowNotice("Connection refused (account: %s, pass: %s, expired ID, ip: %s)\n", sd->userid, sd->passwd, ip);
		return 2; // 2 = This ID is expired
	}

	if( acc.unban_time != 0 && acc.unban_time > time(NULL) )
	{
		strftime(tmpstr, 20, login_config.date_format, localtime(&acc.unban_time));
		tmpstr[19] = '\0';
		ShowNotice("Connection refused (account: %s, pass: %s, banned until %s, ip: %s)\n", sd->userid, sd->passwd, tmpstr, ip);
		return 6; // 6 = Your are Prohibited to log in until %s
	}

	if( acc.state )
	{
		ShowNotice("Connection refused (account: %s, pass: %s, state: %d, ip: %s)\n", sd->userid, sd->passwd, acc.state, ip);
		return acc.state - 1;
	}

	ShowNotice("Authentication accepted (account: %s, id: %d, ip: %s)\n", sd->userid, acc.account_id, ip);

	// update session data
	sd->account_id = acc.account_id;
	sd->login_id1 = rand();
	sd->login_id2 = rand();
	safestrncpy(sd->lastlogin, acc.lastlogin, sizeof(sd->lastlogin));
	sd->sex = acc.sex;
#ifdef TXT_ONLY
	sd->level = isGM(sd->account_id);
#else
	sd->level = acc.level;
#endif

	// update account data
	time(&raw_time);
	strftime(tmpstr, 24, "%Y-%m-%d %H:%M:%S",localtime(&raw_time));
	safestrncpy(acc.lastlogin, tmpstr, sizeof(acc.lastlogin));
	safestrncpy(acc.last_ip, ip, sizeof(acc.last_ip));
	acc.unban_time = 0;
	acc.logincount++;

	accounts->save(accounts, &acc);

	if( sd->sex != 'S' && sd->account_id < START_ACCOUNT_NUM )
		ShowWarning("Account %s has account id %d! Account IDs must be over %d to work properly!\n", sd->userid, sd->account_id, START_ACCOUNT_NUM);

	return -1; // account OK
}

void login_auth_ok(struct login_session_data* sd)
{
	int fd = sd->fd;
	uint32 ip = session[fd]->client_addr;

	uint8 server_num, n;
	uint32 subnet_char_ip;
	struct auth_node* node;
	int i;

	if( sd->level < login_config.min_level_to_connect )
	{
		ShowStatus("Connection refused: the minimum GM level for connection is %d (account: %s, GM level: %d).\n", login_config.min_level_to_connect, sd->userid, sd->level);
		WFIFOHEAD(fd,3);
		WFIFOW(fd,0) = 0x81;
		WFIFOB(fd,2) = 1; // 01 = Server closed
		WFIFOSET(fd,3);
		return;
	}

	server_num = 0;
	for( i = 0; i < MAX_SERVERS; ++i )
		if( session_isValid(server[i].fd) )
			server_num++;

	if( server_num == 0 )
	{// if no char-server, don't send void list of servers, just disconnect the player with proper message
		ShowStatus("Connection refused: there is no char-server online (account: %s).\n", sd->userid);
		WFIFOHEAD(fd,3);
		WFIFOW(fd,0) = 0x81;
		WFIFOB(fd,2) = 1; // 01 = Server closed
		WFIFOSET(fd,3);
		return;
	}

	if( login_config.online_check )
	{
		struct online_login_data* data = (struct online_login_data*)idb_get(online_db, sd->account_id);
		if( data )
		{// account is already marked as online!
			if( data->char_server > -1 )
			{// Request char servers to kick this account out. [Skotlex]
				uint8 buf[6];
				ShowNotice("User '%s' is already online - Rejected.\n", sd->userid);
				WBUFW(buf,0) = 0x2734;
				WBUFL(buf,2) = sd->account_id;
				charif_sendallwos(-1, buf, 6);
				if( data->waiting_disconnect == -1 )
					data->waiting_disconnect = add_timer(gettick()+AUTH_TIMEOUT, waiting_disconnect_timer, sd->account_id, 0);

				WFIFOHEAD(fd,3);
				WFIFOW(fd,0) = 0x81;
				WFIFOB(fd,2) = 8; // 08 = Server still recognizes your last login
				WFIFOSET(fd,3);
				return;
			}
			else
			if( data->char_server == -1 )
			{// client has authed but did not access char-server yet
				// wipe previous session
				idb_remove(auth_db, sd->account_id);
				remove_online_user(sd->account_id);
				data = NULL;
			}
		}
	}

	login_log(ip, sd->userid, 100, "login ok");

	if( sd->level > 0 )
		ShowStatus("Connection of the GM (level:%d) account '%s' accepted.\n", sd->level, sd->userid);
	else
		ShowStatus("Connection of the account '%s' accepted.\n", sd->userid);

	WFIFOHEAD(fd,47+32*server_num);
	WFIFOW(fd,0) = 0x69;
	WFIFOW(fd,2) = 47+32*server_num;
	WFIFOL(fd,4) = sd->login_id1;
	WFIFOL(fd,8) = sd->account_id;
	WFIFOL(fd,12) = sd->login_id2;
	WFIFOL(fd,16) = 0; // in old version, that was for ip (not more used)
	//memcpy(WFIFOP(fd,20), sd->lastlogin, 24); // in old version, that was for name (not more used)
	WFIFOW(fd,44) = 0; // unknown
	WFIFOB(fd,46) = sex_str2num(sd->sex);
	for( i = 0, n = 0; i < MAX_SERVERS; ++i )
	{
		if( !session_isValid(server[i].fd) )
			continue;

		subnet_char_ip = lan_subnetcheck(ip); // Advanced subnet check [LuzZza]
		WFIFOL(fd,47+n*32) = htonl((subnet_char_ip) ? subnet_char_ip : server[i].ip);
		WFIFOW(fd,47+n*32+4) = ntows(htons(server[i].port)); // [!] LE byte order here [!]
		memcpy(WFIFOP(fd,47+n*32+6), server[i].name, 20);
		WFIFOW(fd,47+n*32+26) = server[i].users;
		WFIFOW(fd,47+n*32+28) = server[i].maintenance;
		WFIFOW(fd,47+n*32+30) = server[i].new_;
		n++;
	}
	WFIFOSET(fd,47+32*server_num);

	// create temporary auth entry
	CREATE(node, struct auth_node, 1);
	node->account_id = sd->account_id;
	node->login_id1 = sd->login_id1;
	node->login_id2 = sd->login_id2;
	node->sex = sd->sex;
	node->ip = ip;
	idb_put(auth_db, sd->account_id, node);

	if( login_config.online_check )
	{
		struct online_login_data* data;

		// mark client as 'online'
		data = add_online_user(-1, sd->account_id);

		// schedule deletion of this node
		data->waiting_disconnect = add_timer(gettick()+AUTH_TIMEOUT, waiting_disconnect_timer, sd->account_id, 0);
	}
}

void login_auth_failed(struct login_session_data* sd, int result)
{
	int fd = sd->fd;
	uint32 ip = session[fd]->client_addr;

	if (login_config.log_login)
	{
		const char* error;
		switch( result ) {
		case   0: error = "Unregistered ID."; break; // 0 = Unregistered ID
		case   1: error = "Incorrect Password."; break; // 1 = Incorrect Password
		case   2: error = "Account Expired."; break; // 2 = This ID is expired
		case   3: error = "Rejected from server."; break; // 3 = Rejected from Server
		case   4: error = "Blocked by GM."; break; // 4 = You have been blocked by the GM Team
		case   5: error = "Not latest game EXE."; break; // 5 = Your Game's EXE file is not the latest version
		case   6: error = "Banned."; break; // 6 = Your are Prohibited to log in until %s
		case   7: error = "Server Over-population."; break; // 7 = Server is jammed due to over populated
		case   8: error = "Account limit from company"; break; // 8 = No more accounts may be connected from this company
		case   9: error = "Ban by DBA"; break; // 9 = MSI_REFUSE_BAN_BY_DBA
		case  10: error = "Email not confirmed"; break; // 10 = MSI_REFUSE_EMAIL_NOT_CONFIRMED
		case  11: error = "Ban by GM"; break; // 11 = MSI_REFUSE_BAN_BY_GM
		case  12: error = "Working in DB"; break; // 12 = MSI_REFUSE_TEMP_BAN_FOR_DBWORK
		case  13: error = "Self Lock"; break; // 13 = MSI_REFUSE_SELF_LOCK
		case  14: error = "Not Permitted Group"; break; // 14 = MSI_REFUSE_NOT_PERMITTED_GROUP
		case  15: error = "Not Permitted Group"; break; // 15 = MSI_REFUSE_NOT_PERMITTED_GROUP
		case  99: error = "Account gone."; break; // 99 = This ID has been totally erased
		case 100: error = "Login info remains."; break; // 100 = Login information remains at %s
		case 101: error = "Hacking investigation."; break; // 101 = Account has been locked for a hacking investigation. Please contact the GM Team for more information
		case 102: error = "Bug investigation."; break; // 102 = This account has been temporarily prohibited from login due to a bug-related investigation
		case 103: error = "Deleting char."; break; // 103 = This character is being deleted. Login is temporarily unavailable for the time being
		case 104: error = "Deleting spouse char."; break; // 104 = This character is being deleted. Login is temporarily unavailable for the time being
		default : error = "Unknown Error."; break;
		}

		login_log(ip, sd->userid, result, error);
	}

#ifndef TXT_ONLY
	if( result == 1 ) // failed password
		login_dynamic_ipban(sd->userid, ip);
#endif

	WFIFOHEAD(fd,23);
	WFIFOW(fd,0) = 0x6a;
	WFIFOB(fd,2) = (uint8)result;
	if( result != 6 )
		memset(WFIFOP(fd,3), '\0', 20);
	else
	{// 6 = Your are Prohibited to log in until %s
		struct mmo_account acc;
		char tmpstr[20];
		time_t unban_time = ( accounts->load_str(accounts, &acc, sd->userid) ) ? acc.unban_time : 0;
		strftime(tmpstr, 20, login_config.date_format, localtime(&unban_time));
		safestrncpy((char*)WFIFOP(fd,3), tmpstr, 20); // ban timestamp goes here
	}
	WFIFOSET(fd,23);
}


//----------------------------------------------------------------------------------------
// Default packet parsing (normal players or administation/char-server connection requests)
//----------------------------------------------------------------------------------------
int parse_login(int fd)
{
	struct login_session_data* sd = session[fd]->session_data;
	int result;
	char ip[16];

	if( session[fd]->flag.eof )
	{
		do_close(fd);
		return 0;
	}

	if( sd == NULL ) {
		sd = CREATE(session[fd]->session_data, struct login_session_data, 1);
		sd->fd = fd;
	}

	ip2str(session[fd]->client_addr, ip);

	while( RFIFOREST(fd) >= 2 )
	{
		uint16 command = RFIFOW(fd,0);

		switch( command )
		{

		case 0x0200:		// New alive packet: structure: 0x200 <account.userid>.24B. used to verify if client is always alive.
			if (RFIFOREST(fd) < 26)
				return 0;
			RFIFOSKIP(fd,26);
		break;

		case 0x0204:		// New alive packet: structure: 0x204 <encrypted.account.userid>.16B. (new ragexe from 22 june 2004)
			if (RFIFOREST(fd) < 18)
				return 0;
			RFIFOSKIP(fd,18);
		break;

		case 0x0064:		// request client login
		case 0x01dd:		// request client login (encryption mode)
		case 0x0277:		// New login packet (kRO 2006-04-24aSakexe langtype 0)
		case 0x02b0:		// New login packet (kRO 2007-05-14aSakexe langtype 0)
		{
			size_t packet_len = RFIFOREST(fd); // assume no other packet was sent

			if( (command == 0x0064 && packet_len < 55)
			||  (command == 0x01dd && packet_len < 47)
			||  (command == 0x0277 && packet_len < 84)
			||  (command == 0x02b0 && packet_len < 85) )
				return 0;

			// S 0064 <version>.l <account name>.24B <password>.24B <version2>.B
			// S 01dd <version>.l <account name>.24B <md5 binary>.16B <version2>.B
			// S 0277 <version>.l <account name>.24B <password>.24B <junk?>.29B <version2>.B
			// S 02b0 <version>.l <account name>.24B <password>.24B <junk?>.30B <version2>.B

			sd->version = RFIFOL(fd,2);
			safestrncpy(sd->userid, (char*)RFIFOP(fd,6), NAME_LENGTH);
			if (command != 0x01dd) {
				ShowStatus("Request for connection of %s (ip: %s).\n", sd->userid, ip);
				safestrncpy(sd->passwd, (char*)RFIFOP(fd,30), NAME_LENGTH);
				sd->passwdenc = 0;
			} else {
				ShowStatus("Request for connection (encryption mode) of %s (ip: %s).\n", sd->userid, ip);
				memcpy(sd->passwd, RFIFOP(fd,30), 16); sd->passwd[16] = '\0'; // raw binary data here!
				sd->passwdenc = PASSWORDENC;
			}
			RFIFOSKIP(fd,packet_len);

			result = mmo_auth(sd);

			if( result == -1 )
				login_auth_ok(sd);
			else
				login_auth_failed(sd, result);
		}
		break;

		case 0x01db:	// Sending request of the coding key
		case 0x791a:	// Sending request of the coding key (administration packet)
			RFIFOSKIP(fd,2);
		{
			unsigned int i;

			memset(sd->md5key, '\0', sizeof(sd->md5key));
			sd->md5keylen = (uint16)(12 + rand() % 4);
			for( i = 0; i < sd->md5keylen; ++i )
				sd->md5key[i] = (char)(1 + rand() % 255);

			WFIFOHEAD(fd,4 + sd->md5keylen);
			WFIFOW(fd,0) = 0x01dc;
			WFIFOW(fd,2) = 4 + sd->md5keylen;
			memcpy(WFIFOP(fd,4), sd->md5key, sd->md5keylen);
			WFIFOSET(fd,WFIFOW(fd,2));
		}
		break;

		case 0x2710:	// Connection request of a char-server
			if (RFIFOREST(fd) < 86)
				return 0;
		{
			char server_name[20];
			char message[256];
			uint32 server_ip;
			uint16 server_port;
			uint16 maintenance;
			uint16 new_;

			safestrncpy(sd->userid, (char*)RFIFOP(fd,2), NAME_LENGTH);
			safestrncpy(sd->passwd, (char*)RFIFOP(fd,26), NAME_LENGTH);
			sd->passwdenc = 0;
			sd->version = login_config.client_version_to_connect; // hack to skip version check
			server_ip = ntohl(RFIFOL(fd,54));
			server_port = ntohs(RFIFOW(fd,58));
			safestrncpy(server_name, (char*)RFIFOP(fd,60), 20);
			maintenance = RFIFOW(fd,82);
			new_ = RFIFOW(fd,84);
			RFIFOSKIP(fd,86);

			ShowInfo("Connection request of the char-server '%s' @ %u.%u.%u.%u:%u (account: '%s', pass: '%s', ip: '%s')\n", server_name, CONVIP(server_ip), server_port, sd->userid, sd->passwd, ip);
			sprintf(message, "charserver - %s@%u.%u.%u.%u:%u", server_name, CONVIP(server_ip), server_port);
			login_log(session[fd]->client_addr, sd->userid, 100, message);

			result = mmo_auth(sd);
			if( result == -1 && sd->sex == 'S' && sd->account_id < MAX_SERVERS && server[sd->account_id].fd == -1 )
			{
				ShowStatus("Connection of the char-server '%s' accepted.\n", server_name);
				safestrncpy(server[sd->account_id].name, server_name, sizeof(server[sd->account_id].name));
				server[sd->account_id].fd = fd;
				server[sd->account_id].ip = server_ip;
				server[sd->account_id].port = server_port;
				server[sd->account_id].users = 0;
				server[sd->account_id].maintenance = maintenance;
				server[sd->account_id].new_ = new_;

				session[fd]->func_parse = parse_fromchar;
				session[fd]->flag.server = 1;
				realloc_fifo(fd, FIFOSIZE_SERVERLINK, FIFOSIZE_SERVERLINK);

				// send connection success
				WFIFOHEAD(fd,3);
				WFIFOW(fd,0) = 0x2711;
				WFIFOB(fd,2) = 0;
				WFIFOSET(fd,3);

				// send GM account to char-server
				send_GM_accounts(fd);
			}
			else
			{
				ShowNotice("Connection of the char-server '%s' REFUSED.\n", server_name);
				WFIFOHEAD(fd,3);
				WFIFOW(fd,0) = 0x2711;
				WFIFOB(fd,2) = 3;
				WFIFOSET(fd,3);
			}
		}
		return 0; // processing will continue elsewhere

		case 0x7530:	// Server version information request
			ShowStatus("Sending server version information to ip: %s\n", ip);
			RFIFOSKIP(fd,2);
			WFIFOHEAD(fd,10);
			WFIFOW(fd,0) = 0x7531;
			WFIFOB(fd,2) = ATHENA_MAJOR_VERSION;
			WFIFOB(fd,3) = ATHENA_MINOR_VERSION;
			WFIFOB(fd,4) = ATHENA_REVISION;
			WFIFOB(fd,5) = ATHENA_RELEASE_FLAG;
			WFIFOB(fd,6) = ATHENA_OFFICIAL_FLAG;
			WFIFOB(fd,7) = ATHENA_SERVER_LOGIN;
			WFIFOW(fd,8) = ATHENA_MOD_VERSION;
			WFIFOSET(fd,10);
		break;

		case 0x7918:	// Request for administation login
			if ((int)RFIFOREST(fd) < 4 || (int)RFIFOREST(fd) < ((RFIFOW(fd,2) == 0) ? 28 : 20))
				return 0;
			WFIFOW(fd,0) = 0x7919;
			WFIFOB(fd,2) = 1;
/*
			if( session[fd]->client_addr != admin_allowed_ip ) {
				ShowNotice("'ladmin'-login: Connection in administration mode refused: IP isn't authorised (ladmin_allow, ip: %s).\n", ip);
			} else {
				struct login_session_data *ld = (struct login_session_data*)session[fd]->session_data;
				if (RFIFOW(fd,2) == 0) {	// non encrypted password
					char password[25];
					memcpy(password, RFIFOP(fd,4), 24);
					password[24] = '\0';
					remove_control_chars(password);
					if( !admin_state )
						ShowNotice("'ladmin'-login: Connection in administration mode REFUSED - remote administration is disabled (non encrypted password: %s, ip: %s)\n", password, ip);
					else
					if( strcmp(password, admin_pass) != 0)
						ShowNotice("'ladmin'-login: Connection in administration mode REFUSED - invalid password (non encrypted password: %s, ip: %s)\n", password, ip);
					else {
						// If remote administration is enabled and password sent by client matches password read from login server configuration file
						ShowNotice("'ladmin'-login: Connection in administration mode accepted (non encrypted password: %s, ip: %s)\n", password, ip);
						WFIFOB(fd,2) = 0;
						session[fd]->func_parse = parse_admin;
					}
				} else {	// encrypted password
					if (!ld)
						ShowError("'ladmin'-login: error! MD5 key not created/requested for an administration login.\n");
					else {
						char md5str[64] = "", md5bin[32];
						if (RFIFOW(fd,2) == 1) {
							sprintf(md5str, "%s%s", ld->md5key, admin_pass); // 20 24
						} else if (RFIFOW(fd,2) == 2) {
							sprintf(md5str, "%s%s", admin_pass, ld->md5key); // 24 20
						}
						MD5_String2binary(md5str, md5bin);
						if( !admin_state )
							ShowNotice("'ladmin'-login: Connection in administration mode REFUSED - remote administration is disabled (encrypted password, ip: %s)\n", ip);
						else
						if( memcmp(md5bin, RFIFOP(fd,4), 16) != 0 )
							ShowNotice("'ladmin'-login: Connection in administration mode REFUSED - invalid password (encrypted password, ip: %s)\n", ip);
						else {
							// If remote administration is enabled and password hash sent by client matches hash of password read from login server configuration file
							ShowNotice("'ladmin'-login: Connection in administration mode accepted (encrypted password, ip: %s)\n", ip);
							ShowNotice("Connection of a remote administration accepted (encrypted password).\n");
							WFIFOB(fd,2) = 0;
							session[fd]->func_parse = parse_admin;
						}
					}
				}
			}
*/
			WFIFOSET(fd,3);

			RFIFOSKIP(fd, (RFIFOW(fd,2) == 0) ? 28 : 20);
		break;

		default:
			ShowNotice("Abnormal end of connection (ip: %s): Unknown packet 0x%x\n", ip, command);
			set_eof(fd);
			return 0;
		}
	}

	RFIFOSKIP(fd,RFIFOREST(fd));
	return 0;
}


void login_set_defaults()
{
	login_config.login_ip = INADDR_ANY;
	login_config.login_port = 6900;
	login_config.ip_sync_interval = 0;
	login_config.log_login = true;
	safestrncpy(login_config.date_format, "%Y-%m-%d %H:%M:%S", sizeof(login_config.date_format));
	login_config.console = false;
	login_config.new_account_flag = true;
	login_config.case_sensitive = true;
	login_config.use_md5_passwds = false;
	login_config.login_gm_read = true;
	login_config.min_level_to_connect = 0;
	login_config.online_check = true;
	login_config.check_client_version = false;
	login_config.client_version_to_connect = 20;

	login_config.ipban = true;
	login_config.dynamic_pass_failure_ban = true;
	login_config.dynamic_pass_failure_ban_interval = 5;
	login_config.dynamic_pass_failure_ban_limit = 7;
	login_config.dynamic_pass_failure_ban_duration = 5;
	login_config.use_dnsbl = false;
	safestrncpy(login_config.dnsbl_servs, "", sizeof(login_config.dnsbl_servs));
}

//-----------------------------------
// Reading main configuration file
//-----------------------------------
int login_config_read(const char* cfgName)
{
	char line[1024], w1[1024], w2[1024];
	FILE* fp = fopen(cfgName, "r");
	if (fp == NULL) {
		ShowError("Configuration file (%s) not found.\n", cfgName);
		return 1;
	}
	ShowInfo("Reading configuration file %s...\n", cfgName);
	while(fgets(line, sizeof(line), fp))
	{
		if (line[0] == '/' && line[1] == '/')
			continue;

		if (sscanf(line, "%[^:]: %[^\r\n]", w1, w2) < 2)
			continue;

		if(!strcmpi(w1,"timestamp_format"))
			strncpy(timestamp_format, w2, 20);
		else if(!strcmpi(w1,"stdout_with_ansisequence"))
			stdout_with_ansisequence = config_switch(w2);
		else if(!strcmpi(w1,"console_silent")) {
			ShowInfo("Console Silent Setting: %d\n", atoi(w2));
			msg_silent = atoi(w2);
		}
		else if( !strcmpi(w1, "bind_ip") ) {
			char ip_str[16];
			login_config.login_ip = host2ip(w2);
			if( login_config.login_ip )
				ShowStatus("Login server binding IP address : %s -> %s\n", w2, ip2str(login_config.login_ip, ip_str));
		}
		else if( !strcmpi(w1, "login_port") ) {
			login_config.login_port = (uint16)atoi(w2);
			ShowStatus("set login_port : %s\n",w2);
		}
		else if(!strcmpi(w1, "log_login"))
			login_config.log_login = (bool)config_switch(w2);

		else if(!strcmpi(w1, "new_account"))
			login_config.new_account_flag = (bool)config_switch(w2);
		else if(!strcmpi(w1, "start_limited_time"))
			login_config.start_limited_time = atoi(w2);
		else if(!strcmpi(w1, "check_client_version"))
			login_config.check_client_version = (bool)config_switch(w2);
		else if(!strcmpi(w1, "client_version_to_connect"))
			login_config.client_version_to_connect = atoi(w2);
		else if(!strcmpi(w1, "use_MD5_passwords"))
			login_config.use_md5_passwds = (bool)config_switch(w2);
		else if(!strcmpi(w1, "min_level_to_connect"))
			login_config.min_level_to_connect = atoi(w2);
		else if(!strcmpi(w1, "date_format"))
			safestrncpy(login_config.date_format, w2, sizeof(login_config.date_format));
		else if(!strcmpi(w1, "console"))
			login_config.console = (bool)config_switch(w2);
		else if(!strcmpi(w1, "case_sensitive"))
			login_config.case_sensitive = config_switch(w2);
		else if(!strcmpi(w1, "allowed_regs")) //account flood protection system
			allowed_regs = atoi(w2);
		else if(!strcmpi(w1, "time_allowed"))
			time_allowed = atoi(w2);
		else if(!strcmpi(w1, "online_check"))
			login_config.online_check = (bool)config_switch(w2);
		else if(!strcmpi(w1, "use_dnsbl"))
			login_config.use_dnsbl = (bool)config_switch(w2);
		else if(!strcmpi(w1, "dnsbl_servers"))
			safestrncpy(login_config.dnsbl_servs, w2, sizeof(login_config.dnsbl_servs));
		else if(!strcmpi(w1, "ip_sync_interval"))
			login_config.ip_sync_interval = (unsigned int)1000*60*atoi(w2); //w2 comes in minutes.

#ifdef TXT_ONLY
		else if( login_config_read_txt(w1, w2) )
			continue;
#else
		else if( login_config_read_sql(w1, w2) )
			continue;
#endif

		else if(!strcmpi(w1, "import"))
			login_config_read(w2);
	}
	fclose(fp);
	ShowInfo("Finished reading %s.\n", cfgName);
	return 0;
}

//-----------------------------------
// Reading interserver configuration file
//-----------------------------------
void inter_config_read(const char* cfgName)
{
	char line[1024], w1[1024], w2[1024];
	FILE* fp = fopen(cfgName, "r");
	if(fp == NULL) {
		ShowError("file not found: %s\n", cfgName);
		return;
	}
	ShowInfo("reading configuration file %s...\n", cfgName);
	while(fgets(line, sizeof(line), fp))
	{
		if (line[0] == '/' && line[1] == '/')
			continue;
		if (sscanf(line, "%[^:]: %[^\r\n]", w1, w2) < 2)
			continue;

#ifndef TXT_ONLY
		if( inter_config_read_sql(w1,w2) )
			continue;
#endif
		else if (!strcmpi(w1, "import"))
			inter_config_read(w2);
	}
	fclose(fp);
	ShowInfo("Done reading %s.\n", cfgName);
}


//--------------------------------------
// Function called at exit of the server
//--------------------------------------
void do_final(void)
{
	int i, fd;

	login_log(0, "login server", 100, "login server shutdown");
	ShowStatus("Terminating...\n");

#ifndef TXT_ONLY
	mmo_db_close();
#endif

	accounts->free(accounts);
	online_db->destroy(online_db, NULL);
	auth_db->destroy(auth_db, NULL);

	if(gm_account_db) aFree(gm_account_db);

	for (i = 0; i < MAX_SERVERS; i++) {
		if ((fd = server[i].fd) >= 0) {
			memset(&server[i], 0, sizeof(struct mmo_char_server));
			server[i].fd = -1;
			do_close(fd);
		}
	}
	do_close(login_fd);

	ShowStatus("Finished.\n");
}

//------------------------------
// Function called when the server
// has received a crash signal.
//------------------------------
void do_abort(void)
{
}

void set_server_type(void)
{
	SERVER_TYPE = ATHENA_SERVER_LOGIN;
}

//------------------------------
// Login server initialization
//------------------------------
int do_init(int argc, char** argv)
{
	int i;

	login_set_defaults();

	// read login-server configuration
	login_config_read((argc > 1) ? argv[1] : LOGIN_CONF_NAME);
#ifdef TXT_ONLY
	display_conf_warnings(); // not in login_config_read, because we can use 'import' option, and display same message twice or more
#endif
	inter_config_read(INTER_CONF_NAME);
	login_lan_config_read((argc > 2) ? argv[2] : LAN_CONF_NAME);

	srand((unsigned int)time(NULL));

	for( i = 0; i < MAX_SERVERS; i++ )
		server[i].fd = -1;

#ifndef TXT_ONLY
	mmo_db_init();
#endif

	// Accounts database init
#ifdef TXT_ONLY
	accounts = account_db_txt();
#else
	accounts = account_db_sql();
#endif
	accounts->init(accounts);

	// Online user database init
	online_db = idb_alloc(DB_OPT_RELEASE_DATA);
	add_timer_func_list(waiting_disconnect_timer, "waiting_disconnect_timer");

	// Interserver auth init
	auth_db = idb_alloc(DB_OPT_RELEASE_DATA);

	// Read account information.
#ifdef TXT_ONLY
	read_gm_account();
#else
	if(login_config.login_gm_read)
		read_gm_account();
#endif

	// set default parser as parse_login function
	set_defaultparse(parse_login);

#ifdef TXT_ONLY
	// every x sec we check if gm file has been changed
	if( gm_account_filename_check_timer ) {
		add_timer_func_list(check_GM_file, "check_GM_file");
		add_timer_interval(gettick() + gm_account_filename_check_timer * 1000, check_GM_file, 0, 0, gm_account_filename_check_timer * 1000); 
	}
#else
	// ban deleter timer
	add_timer_func_list(ip_ban_flush, "ip_ban_flush");
	add_timer_interval(gettick()+10, ip_ban_flush, 0, 0, 60*1000);
#endif

	// every 10 minutes cleanup online account db.
	add_timer_func_list(online_data_cleanup, "online_data_cleanup");
	add_timer_interval(gettick() + 600*1000, online_data_cleanup, 0, 0, 600*1000);

	// add timer to detect ip address change and perform update
	if (login_config.ip_sync_interval) {
		add_timer_func_list(sync_ip_addresses, "sync_ip_addresses");
		add_timer_interval(gettick() + login_config.ip_sync_interval, sync_ip_addresses, 0, 0, login_config.ip_sync_interval);
	}

	if( login_config.console )
	{
		//##TODO invoke a CONSOLE_START plugin event
	}

	// server port open & binding
	login_fd = make_listen_bind(login_config.login_ip, login_config.login_port);

	ShowStatus("The login-server is "CL_GREEN"ready"CL_RESET" (Server is listening on the port %u).\n\n", login_config.login_port);
	login_log(0, "login server", 100, "login server started");

	return 0;
}
