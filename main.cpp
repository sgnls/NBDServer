#include <string.h>
#include <cstdlib>
#include <iostream>
#include <errno.h>
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <getopt.h>
#include <fstream>
#include <winsock2.h>
#include <algorithm>
#include <stdint.h>
#include <cstdarg>
#include <vector>

using namespace std;
bool debug=false;
bool allowWrite = false;
string nbdfilename = "";
int partitionNo=0;

//typedef unsigned int UINT;
//typedef unsigned char UCHAR;
//typedef unsigned long ULONG;
//typedef long double LONGDOUBLE;


string
vformat (const char *fmt, va_list ap)
{
    // Allocate a buffer on the stack that's big enough for us almost
    // all the time.
    size_t size = 1024;
    char buf[size];

    // Try to vsnprintf into our buffer.
    va_list apcopy;
    va_copy (apcopy, ap);
    int needed = vsnprintf (&buf[0], size, fmt, ap);
    // NB. On Windows, vsnprintf returns -1 if the string didn't fit the
    // buffer.  On Linux & OSX, it returns the length it would have needed.

    if (needed <= size && needed >= 0) {
        // It fit fine the first time, we're done.
        return std::string (&buf[0]);
    } else {
        // vsnprintf reported that it wanted to write more characters
        // than we allotted.  So do a malloc of the right size and try again.
        // This doesn't happen very often if we chose our initial size
        // well.
        vector <char> buf;
        size = needed;
        buf.resize (size);
        needed = vsnprintf (&buf[0], size, fmt, apcopy);
        return string (&buf[0]);
    }
}

string
sformat (const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    string buf = vformat (fmt, ap);
    va_end (ap);
    return buf;
}



void usage(char *prog)
{
     cout<< prog << " v3.0"<<endl;
     cout<<" -c     Client IP address to accept connections from"<<endl;
     cout<<" -p     Port to listen on (60000 by default)"<<endl;
     cout<<" -f     File to serve ( \\\\.\\PHYSICALDRIVE0: for example)"<<endl;  //escaping \'s should be read as \\.\:
     cout<<" -n     Partition on disk to serve (0 if not specified)"<<endl;
     cout<<" -w     Enable writing (disabled by default)"<<endl;
     cout<<" -d     Enable debug messages"<<endl;
     cout<<" -h     This help text"<<endl;
}

void debugLog(string message){
    if (debug){
    cerr<<message<<endl;
    }
}

int error_mapper(DWORD winerr)
{
	switch(winerr){
	case ERROR_ACCESS_DENIED:
	case ERROR_WRITE_PROTECT:
		return EACCES;

	case ERROR_WRITE_FAULT:
	case ERROR_READ_FAULT:
	case ERROR_GEN_FAILURE:
		return EIO;

	case ERROR_SEEK:
	case ERROR_NEGATIVE_SEEK:
		return ERANGE;

	case ERROR_BAD_UNIT:
	case ERROR_NOT_READY:
	case ERROR_CRC:
	case ERROR_SECTOR_NOT_FOUND:
	case ERROR_DEV_NOT_EXIST:
	case ERROR_DISK_CHANGE:
	case ERROR_BUSY:
	case ERROR_CAN_NOT_COMPLETE:
	case ERROR_UNRECOGNIZED_VOLUME:
	case ERROR_DISK_RECALIBRATE_FAILED:
	case ERROR_DISK_OPERATION_FAILED:
	case ERROR_DISK_RESET_FAILED:
		return EIO;
	}

	return EINVAL; /* what else? */
}

LARGE_INTEGER add_li(LARGE_INTEGER i1, LARGE_INTEGER i2)
{
	LARGE_INTEGER dummy;

	dummy = i1;

	dummy.LowPart += i2.LowPart;
	if (dummy.LowPart <= i1.LowPart && i2.LowPart > 0)
	{
		dummy.HighPart++;
	}

	dummy.HighPart += i2.HighPart;

	return dummy;
}

int READ(SOCKET sh, UCHAR *whereto, int howmuch)
{
	int pnt = 0;

	debugLog(sformat("read: %d bytes requested\n", howmuch));


	while(howmuch > 0)
	{
		int nread = recv(sh, (char *)&whereto[pnt], howmuch, 0);
		if (nread == 0)
			break;
		if (nread == SOCKET_ERROR)
		{
			fprintf(stderr, "Connection dropped. Error: %d\n", WSAGetLastError());
			break;
		}

		pnt += nread;
		howmuch -= nread;
	}

	return pnt;
}

int WRITE(SOCKET sh, UCHAR *wherefrom, int howmuch)
{
	int pnt = 0;

	while(howmuch > 0)
	{
		int nwritten = send(sh, (char *)&wherefrom[pnt], howmuch, 0);
		if (nwritten == 0)
			break;
		if (nwritten == SOCKET_ERROR)
		{
			fprintf(stderr, "Connection dropped. Error: %d\n", WSAGetLastError());
			break;
		}

		pnt += nwritten;
		howmuch -= nwritten;
	}

	return pnt;
}

BOOL getu32(SOCKET sh, ULONG *val)
{
	UCHAR buffer[4];

	if (READ(sh, buffer, 4) != 4)
		return FALSE;

	*val = (buffer[0] << 24) + (buffer[1] << 16) + (buffer[2] << 8) + (buffer[3]);

	return TRUE;
}

BOOL putu32(SOCKET sh, ULONG value)
{
	UCHAR buffer[4];

	buffer[0] = (value >> 24) & 255;
	buffer[1] = (value >> 16) & 255;
	buffer[2] = (value >>  8) & 255;
	buffer[3] = (value      ) & 255;

	if (WRITE(sh, buffer, 4) != 4)
		return FALSE;
	else
		return TRUE;
}




DWORD WINAPI blockServe(LPVOID data){
	SOCKET sockh = (SOCKET)data;
	HANDLE fh;
	LARGE_INTEGER offset, fsize;
	const char *filename;
	filename=nbdfilename.c_str();


	// open file 'filename'
//	fh = CreateFile(filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (allowWrite){
    	fh = CreateFile(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }else{
    	fh = CreateFile(filename, GENERIC_READ , 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    
	if (fh == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Error opening file %s: %d\n", filename, GetLastError());
		goto error;
	}

	// find length of file or starting offset of partition
	memset(&offset, 0x00, sizeof(offset));
	memset(&fsize, 0x00, sizeof(fsize));
	if (strnicmp(filename, "\\\\.\\PHYSICALDRIVE", 17) == 0)	/* disk */
	{
		DWORD dummy2;
		char *dummy = (char *)malloc(4096);
		DRIVE_LAYOUT_INFORMATION *dli = (DRIVE_LAYOUT_INFORMATION *)dummy;
		if (!dummy)
		{
			fprintf(stderr, "Out of memory!\n");
			goto error;
		}
		if (DeviceIoControl(fh, IOCTL_DISK_GET_DRIVE_LAYOUT, NULL, 0, (void *)dli, 4096, &dummy2, NULL) == FALSE)
		{
			fprintf(stderr, "Cannot obtain drive layout: %d\n", GetLastError());
			goto error;
		}

		// find starting offset of partition
		offset = (dli -> PartitionEntry[partitionNo]).StartingOffset;
		fsize  = (dli -> PartitionEntry[partitionNo]).PartitionLength;


		debugLog(sformat("Partition %d is of type %02x\n", partitionNo, (dli -> PartitionEntry[partitionNo]).PartitionType));
		debugLog(sformat("Offset: %ld,%ld (%lx%lx)\n", offset.HighPart, offset.LowPart, offset.HighPart, offset.LowPart));
		debugLog(sformat("Length: %ld,%ld (%lx%lx)\n", fsize.HighPart, fsize.LowPart, fsize.HighPart, fsize.LowPart));

	}
	else													/* file */
	{
		fsize.LowPart = GetFileSize(fh, (DWORD *)&fsize.HighPart);
		if (fsize.LowPart == 0xFFFFFFFF)
		{
			fprintf(stderr, "Failed to obtain filesize info!\n");
			goto error;
		}
	}

	/* negotiate */
	debugLog("Negotiating...sending NBDMAGIC header");
	if (WRITE(sockh, (unsigned char *)"NBDMAGIC", 8) != 8)
	{
		fprintf(stderr, "Failed to send magic string\n");
		goto error;
	}

	// some other magic value
	unsigned char magic[8];
	magic[0] = 0x00;
	magic[1] = 0x00;
	magic[2] = 0x42;
	magic[3] = 0x02;
	magic[4] = 0x81;
	magic[5] = 0x86;
	magic[6] = 0x12;
	magic[7] = 0x53;
	if (WRITE(sockh, magic, 8) != 8)
	{
		fprintf(stderr, "Failed to send 2nd magic string\n");
		goto error;
	}

	// send size of file
	unsigned char exportsize[8];
	exportsize[7] = (fsize.LowPart       ) & 255;	// low word
	exportsize[6] = (fsize.LowPart  >>  8) & 255;
	exportsize[5] = (fsize.LowPart  >> 16) & 255;
	exportsize[4] = (fsize.LowPart  >> 24) & 255;
	exportsize[3] = (fsize.HighPart      ) & 255;	// high word
	exportsize[2] = (fsize.HighPart >>  8) & 255;
	exportsize[1] = (fsize.HighPart >> 16) & 255;
	exportsize[0] = (fsize.HighPart >> 24) & 255;
	if (WRITE(sockh, exportsize, 8) != 8)
	{
		fprintf(stderr, "Failed to send filesize\n");
		goto error;
	}
	
	// send a couple of zeros */
	unsigned char buffer[128];
	memset(buffer, 0x00, 128);
	if (WRITE(sockh, buffer, 128) != 128)
	{
		fprintf(stderr, "Failed to send a couple of 0x00s\n");
		goto error;
	}

	debugLog("Started!");

	/* main loop */
	for(;fh != INVALID_HANDLE_VALUE;)
	{
		UCHAR handle[9];
		ULONG magic, len, type;
		LARGE_INTEGER from;
		LARGE_INTEGER cur_offset;
		int err = 0;

		if (getu32(sockh, &magic) == FALSE ||	// 0x12560953
			getu32(sockh, &type)  == FALSE ||	// 0=read,1=write
			READ(sockh, handle, 8) != 8    ||	// handle
			getu32(sockh, (DWORD *)&from.HighPart) == FALSE ||	// high word of offset
			getu32(sockh, &from.LowPart)  == FALSE ||	// ...low word of offset
			getu32(sockh, &len)   == FALSE)		// length
		{
			fprintf(stderr, "Failed to read from socket\n");
			break;
		}


		handle[8] = 0x00;
		debugLog(sformat("Magic:    %lx\n", magic));
		debugLog(sformat("Offset:   %ld,%ld (%lx%lx)\n", from.HighPart, from.LowPart, from.HighPart, from.LowPart));
		debugLog(sformat("Len:      %ld\n", len));
		debugLog(sformat("Handle:   %s\n", handle));
		debugLog(sformat("Req.type: %ld (%s)\n\n", type, type?"write":"read"));


		// verify protocol
		if (magic != 0x25609513)
		{
			fprintf(stderr, "Unexpected protocol version! (got: %lx, expected: 0x25609513)\n", magic);
			break;
		}

		// calculate current offset
		cur_offset = add_li(offset, from);

		// seek to 'from'
		if (SetFilePointer(fh, cur_offset.LowPart, &cur_offset.HighPart, FILE_BEGIN) == 0xFFFFFFFF)
		{
			fprintf(stderr, "Error seeking in file %s to position %d,%d (%x%x): %d\n", filename,
				cur_offset.HighPart, cur_offset.LowPart, cur_offset.HighPart, cur_offset.LowPart, GetLastError());
			err = error_mapper(GetLastError());
		}

		// error while seeking?
		if (err != 0)
		{
			debugLog(sformat("Sending errno=%d\n", err));
			// send errorstate
			if (putu32(sockh, 0x67446698) == FALSE ||
				putu32(sockh, err) == FALSE ||
				WRITE(sockh, handle, 8) != 8)
			{
				fprintf(stderr, "Failed to send errorstate through socket\n");
				break;
			}
		}
		else if (type == 1)	// write
		{
			while(len > 0)
			{
				DWORD dummy;
				UCHAR buffer[32768];
				// read from socket
				int nb = recv(sockh, (char *)buffer, min((const int)len, (const int)32768), 0);
				if (nb == 0)
					break;

				// write to file;
				if (allowWrite){
    				if (WriteFile(fh, buffer, nb, &dummy, NULL) == 0)
    				{
    					fprintf(stderr, "Failed to write to %s: %d\n", filename, GetLastError());
    					err = error_mapper(GetLastError());
    					break;
    				}
    				if (dummy != nb)
    				{
    					fprintf(stderr, "Failed to write to %s: %d (written: %d, requested to write: %d)\n", filename, GetLastError(), dummy, nb);
    					break;
    				}
                }

				len -= nb;
			}
			if (len)	// connection was closed
			{
				fprintf(stderr, "Connection was dropped while receiving data\n");
				break;
			}

			// send 'ack'
			if (putu32(sockh, 0x67446698) == FALSE ||
				putu32(sockh, err) == FALSE ||
				WRITE(sockh, handle, 8) != 8)
			{
				fprintf(stderr, "Failed to send through socket\n");
				break;
			}
		}
		else if (type == 0)
		{
			// send 'ack'
			if (putu32(sockh, 0x67446698) == FALSE ||
				putu32(sockh, 0) == FALSE ||
				WRITE(sockh, handle, 8) != 8)
			{
				fprintf(stderr, "Failed to send through socket\n");
				break;
			}

			while(len > 0)
			{
				DWORD dummy;
				UCHAR buffer[32768];
				int nb = min((const int)len, (const int)32768);
				int pnt = 0;

				// read nb to buffer;
				if (ReadFile(fh, buffer, nb, &dummy, NULL) == 0)
				{
					fprintf(stderr, "Failed to read from %s: %d\n", filename, GetLastError());
					break;
				}
				if (dummy != nb)
				{
					fprintf(stderr, "Failed to read from %s: %d\n", filename, GetLastError());
					break;
				}

				// send through socket
				if (WRITE(sockh, buffer, nb) != nb) // connection was closed
				{
					fprintf(stderr, "Connection dropped while sending block\n");
					break;
				}

				len -= nb;
			}
			if (len)	// connection was closed
				break;
		}
		else if (type == 2)
		{
            //requested close
            break;
        }			
		else
		{
			fprintf(stderr,"Unexpected commandtype: %d\n", type);
			break;
		}
	}

	// close file
error:
	if (fh != NULL && CloseHandle(fh) == 0)
	{
		fprintf(stderr, "Failed to close handle: %d\n", GetLastError());
	}

	closesocket(sockh);

	ExitThread(0);

	return 0;    
}







int main(int argc, char *argv[])
{
    bool verbose=false;

    char ch;
    string nbdclient = "";
    int port=60000;
    ifstream nbdfile;
    int iError;
    
    while ((ch=getopt(argc,argv,"c:p:f:n:hd")) != EOF)
    switch(ch)
    {
        case 'c':
            nbdclient=optarg;
            break;
        case 'd':
            debug=true;
            break;
        case 'w':
            allowWrite=true;
            break;            
        case 'p':
            port=atoi(optarg);
            break;
        case 'n':
            partitionNo=atoi(optarg);
            break;
        case 'f':
            nbdfilename=optarg;
            break;
        case 'h':
             usage(argv[0]);
             return(0);
        default:
            usage(argv[0]);
            return(-1);
    }
    
    nbdfile.open(nbdfilename.c_str(),ifstream::in|ifstream::binary);
    if ( nbdfile.is_open() )
    {
        debugLog("file opened");
        nbdfile.close();
    }
    else
    {
        cerr<<"error opening file"<<endl;
        return(-1);

    }
    
    //socket init.
   	SOCKET sSock;
	WSADATA wsdata;
	WORD wVersionRequested;
	wVersionRequested = MAKEWORD(2,2);
	iError=WSAStartup(wVersionRequested,&wsdata);
	if (iError != NO_ERROR || iError==1){
        cerr<<"Error initializing winsock.dll"<<endl;
        WSACleanup();
        return(-1);
    }
	sSock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	if ( sSock==INVALID_SOCKET){
        cerr<<"Couldn't open socket..quitting."<<endl;
        WSACleanup();
        return(-1);
    }
	SOCKADDR_IN sServer;
	memset(&sServer,0,sizeof(sServer));
	sServer.sin_family = AF_INET;
	sServer.sin_addr.s_addr = INADDR_ANY;  //listen on any/all IPs.
	sServer.sin_port=htons(port);
	
	//socket options
	int * p_int ;
    p_int = (int*)malloc(sizeof(int));
    *p_int = 1;

    if( (setsockopt(sSock, SOL_SOCKET, SO_REUSEADDR, (char*)p_int, sizeof(int)) == -1 )||
        (setsockopt(sSock, SOL_SOCKET, SO_KEEPALIVE, (char*)p_int, sizeof(int)) == -1 ) ){
        fprintf(stderr,"Error setting options %d\n", WSAGetLastError());
        return(-1);
    }
	
	if (bind(sSock,(LPSOCKADDR) &sServer,sizeof(sServer)) ==SOCKET_ERROR){
        cerr<<"Could not bind socket to server"<<endl;
        return(-1);
    }
    
    //listen and start thread to handle connections
    if (listen(sSock,20)==SOCKET_ERROR){
        cerr<<"Error listening on socket"<<endl;
    }else{
        debugLog("listening...");
    }
    
    while (1){
        debugLog("init socket loop");
        SOCKET sClient;
        struct sockaddr_in  clientAddr;
        int iAddrLen;
        iAddrLen = sizeof(clientAddr);

		/* accept a connection */
		sClient = accept(sSock, (struct sockaddr *)&clientAddr, &iAddrLen);
        if (inet_ntoa(clientAddr.sin_addr)!= nbdclient){
            debugLog("rejecting connection from unauthorized source");
            closesocket(sClient);
        }else if (sClient != INVALID_SOCKET)
		{
            string logMsg ("Connection made with: ");
            logMsg.append(inet_ntoa(clientAddr.sin_addr));
			debugLog(logMsg);
            
            DWORD tid;
			HANDLE th = CreateThread(NULL, 0, blockServe, (void *)sClient, 0, &tid);			
			
        }else{
            cerr<<"Invalid Socket"<<endl;
        }
    
    }
	
	
	
	
	
	


}