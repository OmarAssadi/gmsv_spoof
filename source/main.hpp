#pragma once

#include <string>
#include <GarrysMod/Interfaces.hpp>


#include <dbg.h>
#include <Color.h>

#define DebugMsg( ... ) Msg( __VA_ARGS__ )
#define DebugWarning( ... ) ConColorMsg( 1, global::__yellow, __VA_ARGS__ )


class IServer;

namespace global
{
	extern SourceSDK::FactoryLoader engine_loader;
	extern std::string engine_binary;
	extern IServer *server;


	static Color __yellow( 255, 255, 0, 255 );


}
