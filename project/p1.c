//COMP 4300, Project 1
//Noah Redden, 7841009

#include "physical.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <unistd.h>
#include <sys/timeb.h>

#define lock(l) pthread_mutex_lock(&(l))
#define unlock(l) pthread_mutex_unlock(&(l))

#define WINDOW_SIZE 8
#define MAX_FRAME_NUM 1000
#define TIMEOUT 5

#define ACK 0x01
#define NAK 0x02

// used to keep the DL layer under control
PhysicalState *myState;

pthread_t L2R1;
pthread_t R2L1;
pthread_t L2R2;
pthread_t R2L2;

// file handles used to drive the peers
FILE *L2RFile;
FILE *R2LFile;
FILE *logFile;

void * L2RTx( void *ignored )
{
  printf("Started L2RTx\n");
  fprintf(logFile,"Started L2RTx\n");
  char msg[MAX_FRAME_SIZE];
  int length = 0;

  //sequence number
  int seqNum = 1;//starts at 1, to solve confusion with 0x00 being null byte
  
  int activeFrames = 0;

  //used for ACK recv
  unsigned char buffer[MAX_FRAME_SIZE];
  int index = 0;

  //used for tracking time
  struct timeb tstart, tcurrent;
  int tdelta;

  //backup of old frames
  char pastFrames[MAX_FRAME_NUM][MAX_FRAME_SIZE];

  //transmit 8 frames on sliding window
  while (1)
  {
    // we repeat forever so rewind on EOF
    if (!fgets(msg, MAX_FRAME_SIZE - 1, L2RFile))
      rewind(L2RFile);
    // I don't want to compute this while holding the lock...
    // and I want to transmit the \0 since I'll be printing the received string
    
    //add flags to frame
    char frame[MAX_FRAME_SIZE] = {0x7E, 0xA5};//this is made here because things break otherwise?

    //check if there is a past frame to take from
    if(pastFrames[seqNum-1][0]=='\0'){
  
      //add sequence number
      char seq[2] = {0x00+seqNum, 0x00};//0x00 helps with formatting
      strcat(frame, seq);

      //add data
      strcat(frame,msg);// 0x7EA5 flag to front

      //add end flag
      char post[2] = {0x7E,0xA6};
      strcat(frame, post);// 0x7EA6 flag to back

      //add 16 bit checksum
      int i;
      long sum = 0x0000;
      long bits; //place two chars in here for XOR
      //length+4 should cover up to end flag
      for(i = 0; i < strlen(msg)-1; i+=2){
        //shift together 2 chars.
        bits = msg[i];
        bits = bits << 8;
        bits = bits | msg[i+1];
        //XOR
        sum = sum^bits;
      }
      
      //convert sum back into chars
      char sum1 = sum >> 8;
      char sum2 = sum;
      char checksum[2] = {sum1,sum2};
      strcat(frame, checksum);

      //add to past frames
      strcpy(pastFrames[seqNum-1],frame);
    }
 
    //remeber to change the length;
    length = strlen(pastFrames[seqNum-1])+1;

    lock(myState->L2RLock);
    while (!myState->L2RReady)
      pthread_cond_wait( &(myState->L2RTxSignal), &(myState->L2RLock) );
    
    transmitFrame(L2R, (unsigned char *)pastFrames[seqNum-1], length);
    ftime(&tstart);
    activeFrames++;
    unlock(myState->L2RLock);

    //if window is full, keep accepting ACK until we can let in another frame
    do{
      //recv ACK
      lock(myState->R2LLock);
      while (myState->R2LReady)
        pthread_cond_wait( &(myState->R2LRxSignal), &(myState->R2LLock) );
      
      index = 0;
      
      ftime(&tcurrent);
      //move into a buffer. figure things out outside of the lock
      while (!myState->R2LReady)
        buffer[index++] = receiveByte(R2L);
      unlock(myState->R2LLock);
      
      index = 0;
      
      //check if message starts with flag
      if(buffer[index]==0x7E){
        index++;
        //check if start flag is used
        //cast to unisgned char since A5 is larger than max char
        if((unsigned char)buffer[index]==(unsigned char)0xA5){
          index++;
          int recSeqNum = (int)buffer[index];
          index++;
          
          char byte = buffer[index];
          char ACKbyte = byte;
          index++;
          byte = buffer[index];
          //check for end flag
          if(byte == 0x7E){
            index++;
            byte = buffer[index];
            if((unsigned char)byte==(unsigned char)0xA6){
              //flag is real end flag
              if(ACKbyte==ACK){
                fprintf(logFile,"L2RTx %i: ACK Recieved\n",recSeqNum);
                if(seqNum==recSeqNum){
                  //all is good, next frame
                  seqNum++;
                  activeFrames--;
                }else if(seqNum>recSeqNum){
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  //resend this frame
                  seqNum = recSeqNum;
                  fprintf(logFile,"L2RTx %i: Reset seqNum\n",seqNum);
                  activeFrames--;
                }else{
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  
                  activeFrames--;
                }
              }else{
                fprintf(logFile,"L2RTx %i: NAK Recieved\n",recSeqNum);
                if(seqNum==recSeqNum){
                  //proper NAK. Resend frame
                  activeFrames--;
                }else if(seqNum>recSeqNum){
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  //resend this frame
                  seqNum = recSeqNum;
                  activeFrames--;
                }else{
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  activeFrames--;
                }
              }
              
            }else{
              //should have been end of flag
              fprintf(logFile,"L2RTx %i: Error! No end flag.\n",seqNum);

              //check timeout
              tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
              if(tdelta>TIMEOUT){
                fprintf(logFile,"L2RTx %i: Timeout!.\n", seqNum);
                activeFrames--;
                break;
              }
            }
            
          }else{
            //should have been end of flag
            fprintf(logFile,"L2RTx %i: Error! No end flag.\n",seqNum);  

            //check timeout
            tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
            if(tdelta>TIMEOUT){
              fprintf(logFile,"L2RTx %i: Timeout.\n", seqNum);
              activeFrames--;
              break;
            }
          }            
                    
        }else{
          //not a proper start flag
          fprintf(logFile,"L2RTx %i: Error! No start flag.\n", seqNum);
  
          //check timeout
          tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
          if(tdelta>TIMEOUT){
            fprintf(logFile,"L2RTx %i: Timeout!.\n", seqNum);
            activeFrames--;
            break;
          }
        }
      }else{
        //not a proper start flag
        fprintf(logFile,"L2RTx %i: Error! No start flag.\n", seqNum);

        //check timeout
        tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
        if(tdelta>TIMEOUT){
          fprintf(logFile,"L2RTx %i: Timeout!.\n", seqNum);
          activeFrames--;
          break;
        }
      }
      
    }while(activeFrames>=WINDOW_SIZE);//if too many active frames, ACK until they are gone

  }

  return NULL;
}

void * L2RRx( void *ignored )
{
  printf("Started L2RRx\n");
  fprintf(logFile,"Started L2RRx\n");

  unsigned char buffer[MAX_FRAME_SIZE];
  int index = 0;
  
  //check sum
  long recSum = 0;
  long recBits = 0;

  int seqNum;
  int NFE = 1;

  char ACKbyte;

  while (1)
  {
    lock(myState->R2LLock);
    while (myState->R2LReady)
      pthread_cond_wait( &(myState->R2LRxSignal), &(myState->R2LLock) );
    
    index = 0;
    
    //move into a buffer. figure things out outside of the lock
    while (!myState->R2LReady)
      buffer[index++] = receiveByte(R2L);
    unlock(myState->R2LLock);

    index = 0;
    printf("\n------------------\n");
    //check if message starts with flag
    if(buffer[index]==0x7E){
      index++;
      //check if start flag is used
      //cast to unisgned char since A5 is larger than max char
      if((unsigned char)buffer[index]==(unsigned char)0xA5){
        index++;
        seqNum = (int)buffer[index];
        index++;
        printf("L2RRx %i: ",seqNum);
        
        
        char byte = buffer[index];
        recSum = 0;
        while(1){
          printf("%c",byte);
          //shift together 2 chars.
          recBits = byte;
          recBits = recBits << 8;
          index++;
          byte = buffer[index];
          //check for end flag
          if(byte == 0x7E){
            index++;
            byte = buffer[index];
            if((unsigned char)byte==(unsigned char)0xA6){
              //flag is real end flag, break loop, move to checksum
              break;
            }else{
              //flag was not real, print 0x7E and return to normal
              printf("%c",0x7E);
            }
          }

          printf("%c",byte);

          recBits = recBits | byte;

          //XOR for checksum
          recSum = recSum^recBits;
          index++;
          byte = buffer[index];
          //check for end flag
          if(byte == 0x7E){
            index++;
            byte = buffer[index];
            if((unsigned char)byte==(unsigned char)0xA6){
              //flag is real end flag, break loop, move to checksum
              break;
            }else{
              //flag was not real, print 0x7E and return to normal
              printf("%c",0x7E);
            }
          }
        }
        index++;
        //check checksum
        //shift together 2 chars.
        int bits;
        bits = buffer[index];
        index++;
        bits = bits << 8;
        bits = bits | buffer[index];
        if(bits==recSum){
          //checksum success
          if(NFE==seqNum){
            //correct seq number
            ACKbyte = ACK;
            NFE++;
          }else if(NFE<seqNum){
            fprintf(logFile,"R2LRx %i: Error! Sequence Break! Rec: %i\n",NFE,seqNum);
            ACKbyte = NAK;
            seqNum = NFE;
          }else{
            fprintf(logFile,"R2LRx %i: Error! Sequence Break! Rec: %i\n",NFE,seqNum);
            ACKbyte = NAK;
            NFE = seqNum;
            
          }
        }else{
          //checksum fail
          fprintf(logFile,"R2LRx %i: Error! Message Corrupt! Checksum Fail\n",NFE);
          ACKbyte = NAK;
        }
      }else{
        //not a proper start flag
        fprintf(logFile,"L2RRx %i: Error! No start flag.\n",NFE);
        ACKbyte = NAK;
      }
    }else{
      //not a proper start flag
      fprintf(logFile,"L2RRx %i: Error! No start flag.\n",NFE);
      ACKbyte = NAK;
    }

    printf("\n------------------\n");
    fflush(stdout);

    //send ack
    //add flags to frame
    char frame[MAX_FRAME_SIZE] = {0x7E, 0xA5};

    //add sequence number
    char seq[2] = {0x00+seqNum, ACKbyte};//include ACK byte here
    strcat(frame, seq);

    //add end flag
    char post[2] = {0x7E,0xA6};
    strcat(frame, post);// 0x7EA6 flag to back
 
    //remeber to change the length;
    int length = strlen(frame)+1;

    lock(myState->L2RLock);
    while (!myState->L2RReady)
      pthread_cond_wait( &(myState->L2RTxSignal), &(myState->L2RLock) );
    
    transmitFrame(L2R, (unsigned char *)frame, length);
    unlock(myState->L2RLock);
  }

  return NULL;
}

void * R2LTx( void *ignored )
{
  printf("Started R2LTx\n");
  fprintf(logFile,"Started R2LTx\n");
  char msg[MAX_FRAME_SIZE];
  int length = 0;

  //sequence number
  int seqNum = 1;//starts at 1, to solve confusion with 0x00 being null byte
  
  int activeFrames = 0;

  //used for ACK recv
  unsigned char buffer[MAX_FRAME_SIZE];
  int index = 0;

  //used for tracking time
  struct timeb tstart, tcurrent;
  int tdelta;

  //backup of old frames
  char pastFrames[MAX_FRAME_NUM][MAX_FRAME_SIZE];

  //transmit 8 frames on sliding window
  while (1)
  {
    // we repeat forever so rewind on EOF
    if (!fgets(msg, MAX_FRAME_SIZE - 1, R2LFile))
      rewind(R2LFile);
    // I don't want to compute this while holding the lock...
    // and I want to transmit the \0 since I'll be printing the received string
    
    //add flags to frame
    char frame[MAX_FRAME_SIZE] = {0x7E, 0xA5};//this is made here because things break otherwise?

    //check if there is a past frame to take from
    if(pastFrames[seqNum-1][0]=='\0'){
  
      //add sequence number
      char seq[2] = {0x00+seqNum, 0x00};//0x00 helps with formatting
      strcat(frame, seq);

      //add data
      strcat(frame,msg);// 0x7EA5 flag to front

      //add end flag
      char post[2] = {0x7E,0xA6};
      strcat(frame, post);// 0x7EA6 flag to back

      //add 16 bit checksum
      int i;
      long sum = 0x0000;
      long bits; //place two chars in here for XOR
      //length+4 should cover up to end flag
      for(i = 0; i < strlen(msg)-1; i+=2){
        //shift together 2 chars.
        bits = msg[i];
        bits = bits << 8;
        bits = bits | msg[i+1];
        //XOR
        sum = sum^bits;
      }
      
      //convert sum back into chars
      char sum1 = sum >> 8;
      char sum2 = sum;
      char checksum[2] = {sum1,sum2};
      strcat(frame, checksum);

      //add to past frames
      strcpy(pastFrames[seqNum-1],frame);
    }
 
    //remeber to change the length;
    length = strlen(pastFrames[seqNum-1])+1;

    lock(myState->R2LLock);
    while (!myState->R2LReady)
      pthread_cond_wait( &(myState->R2LTxSignal), &(myState->R2LLock) );
    
    transmitFrame(R2L, (unsigned char *)pastFrames[seqNum-1], length);
    ftime(&tstart);
    activeFrames++;
    unlock(myState->R2LLock);
    
    //if window is full, keep accepting ACK until we can let in another frame
    do{
      //recv ACK
      lock(myState->L2RLock);
      while (myState->L2RReady)
        pthread_cond_wait( &(myState->L2RRxSignal), &(myState->L2RLock) );
      
      index = 0;
      
      ftime(&tcurrent);
      //move into a buffer. figure things out outside of the lock
      while (!myState->L2RReady)
        buffer[index++] = receiveByte(L2R);
      unlock(myState->L2RLock);
      
      index = 0;
      
      //check if message starts with flag
      if(buffer[index]==0x7E){
        index++;
        //check if start flag is used
        //cast to unisgned char since A5 is larger than max char
        if((unsigned char)buffer[index]==(unsigned char)0xA5){
          index++;
          int recSeqNum = (int)buffer[index];
          index++;
          
          char byte = buffer[index];
          char ACKbyte = byte;
          index++;
          byte = buffer[index];
          //check for end flag
          if(byte == 0x7E){
            index++;
            byte = buffer[index];
            if((unsigned char)byte==(unsigned char)0xA6){
              //flag is real end flag
              if(ACKbyte==ACK){
                fprintf(logFile,"L2RTx %i: ACK Recieved\n",recSeqNum);
                if(seqNum==recSeqNum){
                  //all is good, next frame
                  seqNum++;
                  activeFrames--;
                }else if(seqNum>recSeqNum){
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  //resend this frame
                  seqNum = recSeqNum;
                  fprintf(logFile,"L2RTx %i: Reset seqNum\n",seqNum);
                  activeFrames--;
                }else{
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  
                  activeFrames--;
                }
              }else{
                fprintf(logFile,"L2RTx %i: NAK Recieved\n",recSeqNum);
                if(seqNum==recSeqNum){
                  //proper NAK. Resend frame
                  activeFrames--;
                }else if(seqNum>recSeqNum){
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  //resend this frame
                  seqNum = recSeqNum;
                  activeFrames--;
                }else{
                  fprintf(logFile,"L2RTx %i: Error! Sequence Break! Rec: %i\n",seqNum, recSeqNum);
                  activeFrames--;
                }
              }
              
            }else{
              //should have been end of flag
              fprintf(logFile,"L2RTx %i: Error! No end flag.\n",seqNum);

              //check timeout
              tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
              if(tdelta>TIMEOUT){
                fprintf(logFile,"L2RTx %i: Timeout!.\n", seqNum);
                activeFrames--;
                break;
              }
            }
            
          }else{
            //should have been end of flag
            fprintf(logFile,"L2RTx %i: Error! No end flag.\n",seqNum);  

            //check timeout
            tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
            if(tdelta>TIMEOUT){
              fprintf(logFile,"L2RTx %i: Timeout.\n", seqNum);
              activeFrames--;
              break;
            }
          }            
                    
        }else{
          //not a proper start flag
          fprintf(logFile,"L2RTx %i: Error! No start flag.\n", seqNum);
  
          //check timeout
          tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
          if(tdelta>TIMEOUT){
            fprintf(logFile,"L2RTx %i: Timeout!.\n", seqNum);
            activeFrames--;
            break;
          }
        }
      }else{
        //not a proper start flag
        fprintf(logFile,"L2RTx %i: Error! No start flag.\n", seqNum);

        //check timeout
        tdelta = (int) (1000.0*(tcurrent.time - tstart.time)) +(tcurrent.millitm - tstart.millitm);
        if(tdelta>TIMEOUT){
          fprintf(logFile,"L2RTx %i: Timeout!.\n", seqNum);
          activeFrames--;
          break;
        }
      }
      
    }while(activeFrames>=WINDOW_SIZE);//if too many active frames, ACK until they are gone

  }

  return NULL;
}

void * R2LRx( void *ignored )
{
  printf("Started R2LRx\n");
  fprintf(logFile,"Started R2LRx\n");

  unsigned char buffer[MAX_FRAME_SIZE];
  int index = 0;
  
  //check sum
  long recSum = 0;
  long recBits = 0;

  int seqNum;
  int NFE = 1;

  char ACKbyte;

  while (1)
  {
    lock(myState->L2RLock);
    while (myState->L2RReady)
      pthread_cond_wait( &(myState->L2RRxSignal), &(myState->L2RLock) );
    
    index = 0;
    
    //move into a buffer. figure things out outside of the lock
    while (!myState->L2RReady)
      buffer[index++] = receiveByte(L2R);
    unlock(myState->L2RLock);

    index = 0;
    printf("\n------------------\n");
    //check if message starts with flag
    if(buffer[index]==0x7E){
      index++;
      //check if start flag is used
      //cast to unisgned char since A5 is larger than max char
      if((unsigned char)buffer[index]==(unsigned char)0xA5){
        index++;
        seqNum = (uint)buffer[index];
        index++;
        printf("R2LRx %i: ",seqNum);
        
        
        char byte = buffer[index];
        recSum = 0;
        while(1){
          printf("%c",byte);
          //shift together 2 chars.
          recBits = byte;
          recBits = recBits << 8;
          index++;
          byte = buffer[index];
          //check for end flag
          if(byte == 0x7E){
            index++;
            byte = buffer[index];
            if((unsigned char)byte==(unsigned char)0xA6){
              //flag is real end flag, break loop, move to checksum
              break;
            }else{
              //flag was not real, print 0x7E and return to normal
              printf("%c",0x7E);
            }
          }

          printf("%c",byte);

          recBits = recBits | byte;

          //XOR for checksum
          recSum = recSum^recBits;
          index++;
          byte = buffer[index];
          //check for end flag
          if(byte == 0x7E){
            index++;
            byte = buffer[index];
            if((unsigned char)byte==(unsigned char)0xA6){
              //flag is real end flag, break loop, move to checksum
              break;
            }else{
              //flag was not real, print 0x7E and return to normal
              printf("%c",0x7E);
            }
          }
        }
        index++;
        //check checksum
        //shift together 2 chars.
        int bits;
        bits = buffer[index];
        index++;
        bits = bits << 8;
        bits = bits | buffer[index];
        if(bits==recSum){
          //checksum success
          if(NFE==seqNum){
            //correct seq number
            ACKbyte = ACK;
            NFE++;
          }else if(NFE<seqNum){
            fprintf(logFile,"R2LRx %i: Error! Sequence Break! Rec: %i\n",NFE,seqNum);
            ACKbyte = NAK;
            seqNum = NFE;
          }else{
            fprintf(logFile,"R2LRx %i: Error! Sequence Break! Rec: %i\n",NFE,seqNum);
            ACKbyte = NAK;
            NFE = seqNum;
            
          }
        }else{
          //checksum fail
          fprintf(logFile,"R2LRx %i: Error! Message Corrupt! Checksum Fail\n",NFE);
          ACKbyte = NAK;
        }
      }else{
        //not a proper start flag
        fprintf(logFile,"R2LRx %i: Error! No start flag.\n",NFE);
        ACKbyte = NAK;
      }
    }else{
      //not a proper start flag
      fprintf(logFile,"R2LRx %i: Error! No start flag.\n",NFE);
      ACKbyte = NAK;
    }

    printf("\n------------------\n");
    fflush(stdout);

    //send ack
    //add flags to frame
    char frame[MAX_FRAME_SIZE] = {0x7E, 0xA5};

    //add sequence number
    char seq[2] = {0x00+seqNum, ACKbyte};//include ACK byte here
    strcat(frame, seq);

    //add end flag
    char post[2] = {0x7E,0xA6};
    strcat(frame, post);// 0x7EA6 flag to back
 
    //remeber to change the length;
    int length = strlen(frame)+1;

    lock(myState->R2LLock);
    while (!myState->R2LReady)
      pthread_cond_wait( &(myState->R2LTxSignal), &(myState->R2LLock) );
    
    transmitFrame(R2L, (unsigned char *)frame, length);
    unlock(myState->R2LLock);
  }

  return NULL;
}

static void exitTime( int theSignal )
{
  printf("Exiting...\n\n");
  fprintf(logFile,"Exiting...\n\n");

  cleanPhysical();

  fclose(R2LFile);
  fclose(L2RFile);
  fclose(logFile);

  exit(0);
}


int main(int argc, char *argv[])
{
  if (argc == 4)
  {
    printf("Start of Program...\nOpenning %s & %s for messages, %s for logs...\n",argv[1],argv[2],argv[3]);
    L2RFile = fopen(argv[1], "r");
    R2LFile = fopen(argv[2], "r");
    logFile = fopen(argv[3], "w");

    if (L2RFile && R2LFile)
    {
      printf("Successfully opened files.\nStarting threads...\n");
      fprintf(logFile,"Successfully opened files.\nStarting threads...\n");
      
      signal( SIGTERM, exitTime );
      signal( SIGINT, exitTime );

      myState = initPhysical();
      
      pthread_create( &L2R1, NULL, L2RTx, NULL );
      //pthread_create( &R2L1, NULL, R2LTx, NULL );
      //pthread_create( &L2R2, NULL, L2RRx, NULL );
      pthread_create( &R2L2, NULL, R2LRx, NULL );
      
      while (1)
        ;
      
      cleanPhysical();
    }
    else
    {
      printf("Unable to open file\n");
    }
  }
  else
  {
    printf("Usage: L2RFilename R2LFilename logFilename\n");
  }
  
  return EXIT_SUCCESS;
}