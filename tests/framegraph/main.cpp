// Copyright (c) 2018,  Zhirnov Andrey. For more information see 'LICENSE'

#include "FGApp.h"
#include <iostream>

using namespace FG;


int main ()
{
	FGApp::Run();

    FG_LOGI( "FrameGraph integration tests finished" );
	
    std::cin.ignore();
	return 0;
}