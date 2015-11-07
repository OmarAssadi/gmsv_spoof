#include <netfilter.hpp>
#include <main.hpp>
#include <GarrysMod/Lua/LuaInterface.h>
#include <cstdint>
#include <set>
#include <unordered_set>
#include <queue>
#include <string>
#include <eiface.h>
#include <filesystem_stdio.h>
#include <gamemode.h>
#include <iserver.h>
#include <threadtools.h>
#include <utlvector.h>
#include <bitbuf.h>
#include <steam/steamclientpublic.h>
#include <steam/steam_gameserver.h>
#include <interfaces.hpp>
#include <symbolfinder.hpp>
#include <game/server/iplayerinfo.h>

#if defined _WIN32

#include <winsock2.h>

#elif defined __linux || defined __APPLE__

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#endif

namespace netfilter
{

typedef int32_t ( *Hook_recvfrom_t )(
	int32_t s,
	char *buf,
	int32_t buflen,
	int32_t flags,
	sockaddr *from,
	int32_t *fromlen
);

struct packet_t
{
	packet_t( ) :
		address_size( sizeof( address ) )
	{ }

	sockaddr_in address;
	int32_t address_size;
	std::vector<char> buffer;
};

struct netsocket_t
{
	int32_t nPort;
	bool bListening;
	int32_t hUDP;
	int32_t hTCP;
};

struct reply_info_t
{
	std::string game_dir;
	std::string game_version;
	std::string game_desc;
	int32_t max_clients;
	int32_t udp_port;
	std::string tags;
};

// VS2015 compatible (possibly gcc compatible too)
struct gamemode_t
{
	bool _unk1;
	bool _unk2;
	uint16_t _pad;
	std::string name;
	std::string path;
	std::string filters;
	std::string base;
	std::string workshopid;
};

struct query_client_t
{
	bool operator<( const query_client_t &rhs ) const
	{
		return address < rhs.address;
	}

	bool operator==( const query_client_t &rhs ) const
	{
		return address == rhs.address;
	}

	uint32_t address;
	uint32_t last_reset;
	uint32_t count;
};

enum class PacketType
{
	Invalid = -1,
	Good,
	Info
};

typedef CUtlVector<netsocket_t> netsockets_t;

#if defined _WIN32

typedef uintptr_t ( __thiscall *GetGamemode_t )( uintptr_t );

static const char *FileSystemFactory_sym = "\x55\x8B\xEC\x56\x8B\x75\x08\x68\x2A\x2A\x2A\x2A\x56\xE8";
static const size_t FileSystemFactory_symlen = 14;

static const char *NET_ProcessListen_sig = "\x55\x8B\xEC\x83\xEC\x34\x56\x57\x8B\x7D\x08\x8B\xF7\xC1\xE6\x04";
static size_t NET_ProcessListen_siglen = 16;

static const size_t net_sockets_offset = 18;

static const char *IServer_sig = "\x2A\x2A\x2A\x2A\xE8\x2A\x2A\x2A\x2A\xD8\x6D\x24\x83\x4D\xEC\x10";
static const size_t IServer_siglen = 16;

static const uintptr_t GetGamemode_offset = 12;

static const char operating_system_char = 'w';

#elif defined __linux

typedef uintptr_t ( *GetGamemode_t )( uintptr_t );

static const char *FileSystemFactory_sym = "@_Z17FileSystemFactoryPKcPi";
static const size_t FileSystemFactory_symlen = 0;

static const char *NET_ProcessListen_sig = "@_Z17NET_ProcessListeni";
static const size_t NET_ProcessListen_siglen = 0;

static const size_t net_sockets_offset = 36;

static const char *IServer_sig = "\x2A\x2A\x2A\x2A\xE8\x2A\x2A\x2A\x2A\xF3\x0F\x10\x8D\xA8\xFE\xFF";
static const size_t IServer_siglen = 16;

static const uintptr_t GetGamemode_offset = 12;

static const char operating_system_char = 'l';

#elif defined __APPLE__

typedef uintptr_t ( *GetGamemode_t )( uintptr_t );

static const char *FileSystemFactory_sym = "@__Z17FileSystemFactoryPKcPi";
static const size_t FileSystemFactory_symlen = 0;

static const char *NET_ProcessListen_sig = "@__Z17NET_ProcessListeni";
static const size_t NET_ProcessListen_siglen = 0;

static const size_t net_sockets_offset = 23;

static const char *IServer_sig = "\x2A\x2A\x2A\x2A\x8B\x08\x89\x04\x24\xFF\x51\x28\xD9\x9D\x9C\xFE";
static const size_t IServer_siglen = 16;

static const uintptr_t GetGamemode_offset = 20;

static const char operating_system_char = 'm';

#endif

static std::string dedicated_binary = helpers::GetBinaryFileName( "dedicated", false, true, "bin/" );
static SourceSDK::FactoryLoader server_loader( "server", false, true, "garrysmod/bin/" );

static Hook_recvfrom_t Hook_recvfrom = VCRHook_recvfrom;
static int32_t game_socket = -1;

static bool packet_validation_enabled = false;

static bool firewall_whitelist_enabled = false;
static std::unordered_set<uint32_t> firewall_whitelist;

static bool firewall_blacklist_enabled = false;
static std::unordered_set<uint32_t> firewall_blacklist;

static const size_t threaded_socket_max_queue = 1000;
static bool threaded_socket_enabled = false;
static bool threaded_socket_execute = true;
static ThreadHandle_t threaded_socket_handle = nullptr;
static std::queue<packet_t> threaded_socket_queue;

static const char *default_game_version = "15.08.10";
static const uint8_t default_proto_version = 17;
static bool info_cache_enabled = false;
static reply_info_t reply_info;
static char info_cache_buffer[1024] = { 0 };
static bf_write info_cache_packet( info_cache_buffer, sizeof( info_cache_buffer ) );
static uint32_t info_cache_last_update = 0;
static uint32_t info_cache_time = 5;

static const uint32_t query_limiter_max_clients = 4096;
static const uint32_t query_limiter_prune_clients = query_limiter_max_clients * 2 / 3;
static const uint32_t query_limiter_timeout_clients = 120;
static bool query_limiter_enabled = false;
static uint32_t query_limiter_global_count = 0;
static uint32_t query_limiter_global_last_reset = 0;
static std::set<query_client_t> query_limiter_clients;
static uint32_t query_limiter_max_window = 60;
static uint32_t query_limiter_max_sec = 1;
static uint32_t query_limiter_global_max_sec = 50;

static const size_t packet_sampling_max_queue = 10;
static bool packet_sampling_enabled = false;
static std::deque<packet_t> packet_sampling_queue;

static IServer *server = nullptr;
static CGlobalVars *globalvars = nullptr;
static IServerGameDLL *gamedll = nullptr;
static IVEngineServer *engine_server = nullptr;
static IFileSystem *filesystem = nullptr;
static GarrysMod::Lua::ILuaInterface *lua = nullptr;

inline std::string GetGameDescription( )
{
	lua->GetField( GarrysMod::Lua::INDEX_GLOBAL, "hook" );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::TABLE ) )
	{
		lua->Pop( 1 );
		return "";
	}

	lua->GetField( -1, "Run" );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::FUNCTION ) )
	{
		lua->Pop( 2 );
		return "";
	}

	lua->PushString( "GetGameDescription" );
	if( lua->PCall( 1, 1, 0 ) != 0 || !lua->IsType( -1, GarrysMod::Lua::Type::STRING ) )
	{
		lua->Pop( 2 );
		return "";
	}

	std::string gamedesc = lua->GetString( -1 );
	lua->Pop( 2 );
	return gamedesc;
}

static void BuildStaticReplyInfo( )
{
	reply_info.game_desc = GetGameDescription( );

	{
		reply_info.game_dir.resize( 256 );
		engine_server->GetGameDir( &reply_info.game_dir[0], reply_info.game_dir.size( ) );
		reply_info.game_dir.resize( strlen( reply_info.game_dir.c_str( ) ) );

		size_t pos = reply_info.game_dir.find_last_of( "\\/" );
		if( pos != reply_info.game_dir.npos )
			reply_info.game_dir.erase( 0, pos + 1 );
	}

	reply_info.max_clients = server->GetMaxClients( );

	reply_info.udp_port = server->GetUDPPort( );

	{
		Gamemode::System *gamemodes = reinterpret_cast<CFileSystem_Stdio *>( filesystem )->Gamemodes( );
		gamemode_t *gamemode = static_cast<gamemode_t *>( gamemodes->Active( ) );

		reply_info.tags = " gm:";
		reply_info.tags += gamemode->path;

		if( !gamemode->workshopid.empty( ) )
		{
			reply_info.tags += " gmws:";
			reply_info.tags += gamemode->workshopid;
		}
	}

	{
		FileHandle_t file = filesystem->Open( "steam.inf", "r", "GAME" );
		if( file == nullptr )
		{
			reply_info.game_version = default_game_version;
			DebugWarning( "[ServerSecure] Error opening steam.inf\n" );
			return;
		}

		char buff[256] = { 0 };
		bool failed = filesystem->ReadLine( buff, sizeof( buff ), file ) == nullptr;
		filesystem->Close( file );
		if( failed )
		{
			reply_info.game_version = default_game_version;
			DebugWarning( "[ServerSecure] Failed reading steam.inf\n" );
			return;
		}

		reply_info.game_version = &buff[13];

		size_t pos = reply_info.game_version.find_first_of( "\r\n" );
		if( pos != reply_info.game_version.npos )
			reply_info.game_version.erase( pos );
	}
}

// maybe divide into low priority and high priority data?
// low priority would be VAC protection status for example
// updated on a much bigger period
static void BuildReplyInfo( )
{
	info_cache_packet.Reset( );

	info_cache_packet.WriteLong( -1 ); // connectionless packet header
	info_cache_packet.WriteByte( 'I' ); // packet type is always 'I'
	info_cache_packet.WriteByte( default_proto_version );
	info_cache_packet.WriteString( server->GetName( ) );
	info_cache_packet.WriteString( server->GetMapName( ) );
	info_cache_packet.WriteString( reply_info.game_dir.c_str( ) );
	info_cache_packet.WriteString( reply_info.game_desc.c_str( ) );

	int32_t appid = engine_server->GetAppID( );
	info_cache_packet.WriteShort( appid );

	info_cache_packet.WriteByte( server->GetNumClients( ) );
	info_cache_packet.WriteByte( reply_info.max_clients );
	info_cache_packet.WriteByte( server->GetNumFakeClients( ) );
	info_cache_packet.WriteByte( 'd' ); // dedicated server identifier
	info_cache_packet.WriteByte( operating_system_char );
	info_cache_packet.WriteByte( server->GetPassword( ) != nullptr ? 1 : 0 );
	// if vac protected, it activates itself some time after startup
	info_cache_packet.WriteByte( SteamGameServer_BSecure( ) );
	info_cache_packet.WriteString( reply_info.game_version.c_str( ) );

	const CSteamID *sid = engine_server->GetGameServerSteamID( );
	uint64_t steamid = 0;
	if( sid != nullptr )
		steamid = sid->ConvertToUint64( );

	if( reply_info.tags.empty( ) )
	{
		// 0x80 - port number is present
		// 0x10 - server steamid is present
		// 0x01 - game long appid is present
		info_cache_packet.WriteByte( 0x80 | 0x10 | 0x01 );
		info_cache_packet.WriteShort( reply_info.udp_port );
		info_cache_packet.WriteLongLong( steamid );
		info_cache_packet.WriteLongLong( appid );
	}
	else
	{
		// 0x80 - port number is present
		// 0x10 - server steamid is present
		// 0x20 - tags are present
		// 0x01 - game long appid is present
		info_cache_packet.WriteByte( 0x80 | 0x10 | 0x20 | 0x01 );
		info_cache_packet.WriteShort( reply_info.udp_port );
		info_cache_packet.WriteLongLong( steamid );
		info_cache_packet.WriteString( reply_info.tags.c_str( ) );
		info_cache_packet.WriteLongLong( appid );
	}
}

inline bool CheckIPRate( const sockaddr_in &from, uint32_t time )
{
	if( !query_limiter_enabled )
		return true;

	if( query_limiter_clients.size( ) >= query_limiter_max_clients )
		for( auto it = query_limiter_clients.begin( ); it != query_limiter_clients.end( ); ++it )
		{
			const query_client_t &client = *it;
			if( client.last_reset - time >= query_limiter_timeout_clients && client.address != from.sin_addr.s_addr )
			{
				query_limiter_clients.erase( it );

				if( query_limiter_clients.size( ) <= query_limiter_prune_clients )
					break;
			}
		}

	query_client_t client = { from.sin_addr.s_addr, time, 1 };
	auto it = query_limiter_clients.find( client );
	if( it != query_limiter_clients.end( ) )
	{
		client = *it;
		query_limiter_clients.erase( it );

		if( time - client.last_reset >= query_limiter_max_window )
		{
			client.last_reset = time;
		}
		else
		{
			++client.count;
			if( client.count / query_limiter_max_window >= query_limiter_max_sec )
			{
				query_limiter_clients.insert( client );
				DebugWarning(
					"[ServerSecure] %s reached its query limit!\n",
					inet_ntoa( from.sin_addr )
				);
				return false;
			}
		}
	}

	query_limiter_clients.insert( client );

	if( time - query_limiter_global_last_reset > query_limiter_max_window )
	{
		query_limiter_global_last_reset = time;
		query_limiter_global_count = 1;
	}
	else
	{
		++query_limiter_global_count;
		if( query_limiter_global_count / query_limiter_max_window >= query_limiter_global_max_sec )
		{
			DebugWarning(
				"[ServerSecure] %s reached the global query limit!\n",
				inet_ntoa( from.sin_addr )
			);
			return false;
		}
	}

	return true;
}

inline PacketType SendInfoCache( const sockaddr_in &from, uint32_t time )
{
	if( time - info_cache_last_update >= info_cache_time )
	{
		BuildReplyInfo( );
		info_cache_last_update = time;
	}

	sendto(
		game_socket,
		reinterpret_cast<char *>( info_cache_packet.GetData( ) ),
		info_cache_packet.GetNumBytesWritten( ),
		0,
		reinterpret_cast<const sockaddr *>( &from ),
		sizeof( from )
	);

	return PacketType::Invalid; // we've handled it
}

static PacketType HandleInfoQuery( const sockaddr_in &from )
{
	uint32_t time = static_cast<uint32_t>( globalvars->realtime );
	if( !CheckIPRate( from, time ) )
		return PacketType::Invalid;

	if( info_cache_enabled )
		return SendInfoCache( from, time );

	return PacketType::Good;
}

static PacketType ClassifyPacket( const char *data, int32_t len, const sockaddr_in &from )
{
	if( len == 0 )
	{
		DebugWarning(
			"[ServerSecure] Bad OOB! len: %d from %s\n",
			len,
			inet_ntoa( from.sin_addr )
		);
		return PacketType::Invalid;
	}

	if( len < 5 )
		return PacketType::Good;

	int32_t channel = *reinterpret_cast<const int32_t *>( data );
	if( channel == -2 )
	{
		DebugWarning(
			"[ServerSecure] Bad OOB! len: %d, channel: 0x%X from %s\n",
			len,
			channel,
			inet_ntoa( from.sin_addr )
		);
		return PacketType::Invalid;
	}

	if( channel != -1 )
		return PacketType::Good;

	uint8_t type = *reinterpret_cast<const uint8_t *>( data + 4 );
	if( packet_validation_enabled )
	{
		switch( type )
		{
			case 'W': // server challenge request
			case 's': // master server challenge
				if( len > 100 )
				{
					DebugWarning(
						"[ServerSecure] Bad OOB! len: %d, channel: 0x%X, type: %c from %s\n",
						len,
						channel,
						type,
						inet_ntoa( from.sin_addr )
					);
					return PacketType::Invalid;
				}

				if( len >= 18 && strncmp( data + 5, "statusResponse", 14 ) == 0 )
				{
					DebugWarning(
						"[ServerSecure] Bad OOB! len: %d, channel: 0x%X, type: %c from %s\n",
						len,
						channel,
						type,
						inet_ntoa( from.sin_addr )
					);
					return PacketType::Invalid;
				}

				return PacketType::Good;

			case 'T': // server info request
				return len == 25 && strncmp( data + 5, "Source Engine Query", 19 ) == 0 ?
					PacketType::Info : PacketType::Invalid;

			case 'U': // player info request
			case 'V': // rules request
				return len == 9 ? PacketType::Good : PacketType::Invalid;

			case 'q': // connection handshake init
			case 'k': // steam auth packet
				DebugMsg(
					"[ServerSecure] Good OOB! len: %d, channel: 0x%X, type: %c from %s\n",
					len,
					channel,
					type,
					inet_ntoa( from.sin_addr )
				);
				return PacketType::Good;
		}

		DebugWarning(
			"[ServerSecure] Bad OOB! len: %d, channel: 0x%X, type: %c from %s\n",
			len,
			channel,
			type,
			inet_ntoa( from.sin_addr )
		);
		return PacketType::Invalid;
	}

	return type == 'T' ? PacketType::Info : PacketType::Good;
}

inline bool IsAddressAllowed( const sockaddr_in &addr )
{
	return
		( !firewall_whitelist_enabled ||
		firewall_whitelist.find( addr.sin_addr.s_addr ) != firewall_whitelist.end( ) ) &&
		( !firewall_blacklist_enabled ||
			firewall_blacklist.find( addr.sin_addr.s_addr ) == firewall_blacklist.end( ) );
}

inline int32_t HandleNetError( int32_t value )
{
	if( value == -1 )

#if defined _WIN32

		WSASetLastError( WSAEWOULDBLOCK );

#elif defined __linux || defined __APPLE__

		errno = EWOULDBLOCK;

#endif

	return value;
}

inline packet_t GetQueuedPacket( )
{
	//bool full = threaded_socket_queue.size( ) >= thread_socket_max_queue;
	packet_t p = threaded_socket_queue.front( );
	threaded_socket_queue.pop( );
	// maybe have an algorithm for when the queue is full
	// drop certain packet types like queries
	return p;
}

inline int32_t ReceiveAndAnalyzePacket(
	int32_t s,
	char *buf,
	int32_t buflen,
	int32_t flags,
	sockaddr *from,
	int32_t *fromlen
)
{
	sockaddr_in &infrom = *reinterpret_cast<sockaddr_in *>( from );
	int32_t len = Hook_recvfrom( s, buf, buflen, flags, from, fromlen );
	if( len == -1 )
		return -1;

	if( packet_sampling_enabled )
	{
		// there should only be packet_sampling_max_queue packets on the queue at the moment of this check
		if( packet_sampling_queue.size( ) >= packet_sampling_max_queue )
			packet_sampling_queue.pop_front( );

		packet_t p;
		memcpy( &p.address, from, *fromlen );
		p.address_size = *fromlen;
		p.buffer.assign( buf, buf + len );
		packet_sampling_queue.push_back( p );
	}

	if( !IsAddressAllowed( infrom ) )
		return -1;

	PacketType type = ClassifyPacket( buf, len, infrom );
	if( type == PacketType::Info )
		type = HandleInfoQuery( infrom );

	if( type == PacketType::Invalid )
		return -1;

	return len;
}

static int32_t Hook_recvfrom_d(
	int32_t s,
	char *buf,
	int32_t buflen,
	int32_t flags,
	sockaddr *from,
	int32_t *fromlen
)
{
	if( !threaded_socket_enabled && threaded_socket_queue.empty( ) )
		return HandleNetError( ReceiveAndAnalyzePacket( s, buf, buflen, flags, from, fromlen ) );

	if( threaded_socket_queue.empty( ) )
		return HandleNetError( -1 );

	packet_t p = GetQueuedPacket( );
	int32_t len = static_cast<int32_t>( p.buffer.size( ) );
	if( len > buflen )
		len = buflen;

	size_t addrlen = static_cast<size_t>( *fromlen );
	if( addrlen > sizeof( p.address ) )
		addrlen = sizeof( p.address );

	memcpy( buf, p.buffer.data( ), len );
	memcpy( from, &p.address, addrlen );
	*fromlen = p.address_size;

	return len;
}

static uint32_t Hook_recvfrom_thread( void *param )
{
	timeval ms100 = { 0, 100000 };
	char tempbuf[65535] = { 0 };
	fd_set readables;

	while( threaded_socket_execute )
	{
		if( !threaded_socket_enabled || threaded_socket_queue.size( ) >= threaded_socket_max_queue )
		// testing for maximum queue size, this is a very cheap "fix"
		// the socket itself has a queue too but will start dropping packets when full
		{
			ThreadSleep( 100 );
			continue;
		}

		FD_ZERO( &readables );
		FD_SET( game_socket, &readables );
		if( select( game_socket + 1, &readables, nullptr, nullptr, &ms100 ) == -1 || !FD_ISSET( game_socket, &readables ) )
			continue;

		packet_t p;
		int32_t len = ReceiveAndAnalyzePacket(
			game_socket,
			tempbuf,
			sizeof( tempbuf ),
			0,
			reinterpret_cast<sockaddr *>( &p.address ),
			&p.address_size
		);
		if( len == -1 )
			continue;

		p.buffer.assign( tempbuf, tempbuf + len );
		threaded_socket_queue.push( p );
	}

	return 0;
}

inline void SetDetourStatus( bool enabled )
{
	if( enabled )
		VCRHook_recvfrom = Hook_recvfrom_d;
	else if( !firewall_whitelist_enabled &&
		!firewall_blacklist_enabled &&
		!packet_validation_enabled &&
		!threaded_socket_enabled )
		VCRHook_recvfrom = Hook_recvfrom;
}

LUA_FUNCTION_STATIC( EnableFirewallWhitelist )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );
	firewall_whitelist_enabled = LUA->GetBool( 1 );
	SetDetourStatus( firewall_whitelist_enabled );
	return 0;
}

// Whitelisted IPs bytes need to be in network order (big endian)
LUA_FUNCTION_STATIC( AddWhitelistIP )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	firewall_whitelist.insert( static_cast<uint32_t>( LUA->GetNumber( 1 ) ) );
	return 0;
}

LUA_FUNCTION_STATIC( RemoveWhitelistIP )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	firewall_whitelist.erase( static_cast<uint32_t>( LUA->GetNumber( 1 ) ) );
	return 0;
}

LUA_FUNCTION_STATIC( ResetWhitelist )
{
	std::unordered_set<uint32_t>( ).swap( firewall_whitelist );
	return 0;
}

LUA_FUNCTION_STATIC( EnableFirewallBlacklist )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );
	firewall_blacklist_enabled = LUA->GetBool( 1 );
	SetDetourStatus( firewall_blacklist_enabled );
	return 0;
}

// Blacklisted IPs bytes need to be in network order (big endian)
LUA_FUNCTION_STATIC( AddBlacklistIP )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	firewall_blacklist.insert( static_cast<uint32_t>( LUA->GetNumber( 1 ) ) );
	return 0;
}

LUA_FUNCTION_STATIC( RemoveBlacklistIP )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	firewall_blacklist.erase( static_cast<uint32_t>( LUA->GetNumber( 1 ) ) );
	return 0;
}

LUA_FUNCTION_STATIC( ResetBlacklist )
{
	std::unordered_set<uint32_t>( ).swap( firewall_blacklist );
	return 0;
}

LUA_FUNCTION_STATIC( EnablePacketValidation )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );
	packet_validation_enabled = LUA->GetBool( 1 );
	SetDetourStatus( packet_validation_enabled );
	return 0;
}

LUA_FUNCTION_STATIC( EnableThreadedSocket )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );
	threaded_socket_enabled = LUA->GetBool( 1 );
	SetDetourStatus( threaded_socket_enabled );
	return 0;
}

LUA_FUNCTION_STATIC( EnableInfoCache )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );
	info_cache_enabled = LUA->GetBool( 1 );
	return 0;
}

LUA_FUNCTION_STATIC( SetInfoCacheTime )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	info_cache_time = static_cast<uint32_t>( LUA->GetNumber( 1 ) );
	return 0;
}

LUA_FUNCTION_STATIC( RefreshInfoCache )
{
	BuildStaticReplyInfo( );
	BuildReplyInfo( );
	return 0;
}

LUA_FUNCTION_STATIC( EnableQueryLimiter )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );
	query_limiter_enabled = LUA->GetBool( 1 );
	return 0;
}

LUA_FUNCTION_STATIC( SetMaxQueriesWindow )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	query_limiter_max_window = static_cast<uint32_t>( LUA->GetNumber( 1 ) );
	return 0;
}

LUA_FUNCTION_STATIC( SetMaxQueriesPerSecond )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	query_limiter_max_sec = static_cast<uint32_t>( LUA->GetNumber( 1 ) );
	return 0;
}

LUA_FUNCTION_STATIC( SetGlobalMaxQueriesPerSecond )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::NUMBER );
	query_limiter_global_max_sec = static_cast<uint32_t>( LUA->GetNumber( 1 ) );
	return 0;
}

LUA_FUNCTION_STATIC( EnablePacketSampling )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );

	packet_sampling_enabled = LUA->GetBool( 1 );
	if( !packet_sampling_enabled )
		packet_sampling_queue.clear( );

	return 0;
}

LUA_FUNCTION_STATIC( GetSamplePacket )
{
	if( packet_sampling_queue.empty( ) )
		return 0;

	packet_t p = packet_sampling_queue.front( );
	packet_sampling_queue.pop_front( );
	LUA->PushNumber( p.address.sin_addr.s_addr );
	LUA->PushNumber( p.address.sin_port );
	LUA->PushString( p.buffer.data( ), p.buffer.size( ) );
	return 3;
}

void Initialize( lua_State *state )
{
	lua = static_cast<GarrysMod::Lua::ILuaInterface *>( LUA );

	if( !server_loader.IsValid( ) )
		LUA->ThrowError( "unable to get server factory" );

	gamedll = server_loader.GetInterface<IServerGameDLL>( INTERFACEVERSION_SERVERGAMEDLL_VERSION_9 );
	if( gamedll == nullptr )
		LUA->ThrowError( "failed to load required IServerGameDLL interface" );

	engine_server = global::engine_loader.GetInterface<IVEngineServer>(
		INTERFACEVERSION_VENGINESERVER_VERSION_21
	);
	if( engine_server == nullptr )
		LUA->ThrowError( "failed to load required IVEngineServer interface" );

	IPlayerInfoManager *playerinfo = server_loader.GetInterface<IPlayerInfoManager>(
		INTERFACEVERSION_PLAYERINFOMANAGER
	);
	if( playerinfo == nullptr )
		LUA->ThrowError( "failed to load required IPlayerInfoManager interface" );

	globalvars = playerinfo->GetGlobalVars( );
	if( globalvars == nullptr )
		LUA->ThrowError( "failed to load required CGlobalVars interface" );

	SymbolFinder symfinder;

	CreateInterfaceFn factory = reinterpret_cast<CreateInterfaceFn>( symfinder.ResolveOnBinary(
		dedicated_binary.c_str( ), FileSystemFactory_sym, FileSystemFactory_symlen
	) );
	if( factory == nullptr )
		LUA->ThrowError( "unable to retrieve dedicated factory" );

	filesystem = static_cast<IFileSystem *>( factory(
		FILESYSTEM_INTERFACE_VERSION,
		nullptr
	) );
	if( filesystem == nullptr )
		LUA->ThrowError( "failed to initialize IFileSystem" );

	IServer **pserver = reinterpret_cast<IServer **>( symfinder.ResolveOnBinary(
		global::engine_lib.c_str( ),
		IServer_sig,
		IServer_siglen
	) );
	if( pserver == nullptr )
		LUA->ThrowError( "failed to locate IServer pointer" );

	server = *pserver;
	if( server == nullptr )
		LUA->ThrowError( "failed to locate IServer" );

	uint8_t *net_sockets_pointer = reinterpret_cast<uint8_t *>( symfinder.ResolveOnBinary(
		global::engine_lib.c_str( ),
		NET_ProcessListen_sig,
		NET_ProcessListen_siglen
	) );
	if( net_sockets_pointer == nullptr )
		LUA->ThrowError( "unable to sigscan for net_sockets" );

	netsockets_t *net_sockets = *reinterpret_cast<netsockets_t **>(
		net_sockets_pointer + net_sockets_offset
	);
	if( net_sockets == nullptr )
		LUA->ThrowError( "got an invalid pointer to net_sockets" );

	game_socket = net_sockets->Element( 1 ).hUDP;
	if( game_socket == -1 )
		LUA->ThrowError( "got an invalid server socket" );

	threaded_socket_execute = true;
	threaded_socket_handle = CreateSimpleThread( Hook_recvfrom_thread, nullptr );
	if( threaded_socket_handle == nullptr )
		LUA->ThrowError( "unable to create thread" );

	BuildStaticReplyInfo( );

	LUA->PushCFunction( EnableFirewallWhitelist );
	LUA->SetField( -2, "EnableFirewallWhitelist" );

	LUA->PushCFunction( AddWhitelistIP );
	LUA->SetField( -2, "AddWhitelistIP" );

	LUA->PushCFunction( RemoveWhitelistIP );
	LUA->SetField( -2, "RemoveWhitelistIP" );

	LUA->PushCFunction( ResetWhitelist );
	LUA->SetField( -2, "ResetWhitelist" );

	LUA->PushCFunction( EnableFirewallBlacklist );
	LUA->SetField( -2, "EnableFirewallBlacklist" );

	LUA->PushCFunction( AddBlacklistIP );
	LUA->SetField( -2, "AddBlacklistIP" );

	LUA->PushCFunction( RemoveBlacklistIP );
	LUA->SetField( -2, "RemoveBlacklistIP" );

	LUA->PushCFunction( ResetBlacklist );
	LUA->SetField( -2, "ResetBlacklist" );

	LUA->PushCFunction( EnablePacketValidation );
	LUA->SetField( -2, "EnablePacketValidation" );

	LUA->PushCFunction( EnableThreadedSocket );
	LUA->SetField( -2, "EnableThreadedSocket" );

	LUA->PushCFunction( EnableInfoCache );
	LUA->SetField( -2, "EnableInfoCache" );

	LUA->PushCFunction( SetInfoCacheTime );
	LUA->SetField( -2, "SetInfoCacheTime" );

	LUA->PushCFunction( RefreshInfoCache );
	LUA->SetField( -2, "RefreshInfoCache" );

	LUA->PushCFunction( EnableQueryLimiter );
	LUA->SetField( -2, "EnableQueryLimiter" );

	LUA->PushCFunction( SetMaxQueriesWindow );
	LUA->SetField( -2, "SetMaxQueriesWindow" );

	LUA->PushCFunction( SetMaxQueriesPerSecond );
	LUA->SetField( -2, "SetMaxQueriesPerSecond" );

	LUA->PushCFunction( SetGlobalMaxQueriesPerSecond );
	LUA->SetField( -2, "SetGlobalMaxQueriesPerSecond" );

	LUA->PushCFunction( EnablePacketSampling );
	LUA->SetField( -2, "EnablePacketSampling" );

	LUA->PushCFunction( GetSamplePacket );
	LUA->SetField( -2, "GetSamplePacket" );
}

void Deinitialize( lua_State * )
{
	if( threaded_socket_handle != nullptr )
	{
		threaded_socket_execute = false;
		ThreadJoin( threaded_socket_handle );
		ReleaseThreadHandle( threaded_socket_handle );
		threaded_socket_handle = nullptr;
	}

	VCRHook_recvfrom = Hook_recvfrom;
}

}
