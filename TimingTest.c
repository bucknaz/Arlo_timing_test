/*
  Blank Simple Project.c
  http://learn.parallax.com/propeller-c-tutorials 
*/


#include "simpletools.h"                      // Include simple tools
#include "fdserial.h"

#include "per_robot_settings_for_propeller_c_code.h"
#include "Arlo_Ping.h"
#include "Arlo_SafetyOverride.h"
#include "Arlo_DBH-10.h"
#include "Arlo_Ir.h"
#include "Arlo_Gyro.h"
#include "sequencer.h"





//Global vars used within this file

// This is the ROS io port
fdserial *term;

//used for input string processing
#define RXBUFFERLEN 40
static char rx_buf[RXBUFFERLEN];// A Buffer long enough to hold the longest line ROS may send.
char in_buf[RXBUFFERLEN];// A Buffer long enough to hold the longest line ROS may send.
static int rx_count = 0;
volatile int got_one = 0;
static int dbh_Error = 0;


#ifdef hasGyro
static double gyroHeading = 0.0;
#endif


//Params that can be sent when initializing
double Heading = 0.0;
double X = 0.0;
double Y = 0.0;
double distancePerCount = 0.0;
double trackWidth = 0.0;
int robotInitialized=0;

//These get set in ping, I know wrong place
double BatteryVolts=12.0;
double RawBatVolts=4.0;


//Are twist comands
double CommandedVelocity = 0.0;
double CommandedAngularVelocity = 0.0;
double angularVelocityOffset = 0.0;


// used in parse input, printed in main and adjusted in safty override cog
volatile int abd_speedLimit = MAXIMUM_SPEED;
volatile int abdR_speedLimit = MAXIMUM_SPEED; // Reverse speed limit to allow robot to reverse fast if it is blocked in front and visa versa




//Stack for ros emulater
#ifdef EMULATE_ROS
volatile double ros_t=0;
volatile double ros_v=0;
volatile double ros_r=.51;

uint32_t rstack[80];
void ROS();
#endif

int free_cogs()
{
  int free = 0;;
  int i;
  int cog[8];
  int jmp_0 = 0b01011100011111000000000000000000;

  while (( cog[free] = cognew(&jmp_0, 0)) >= 0) 
    free++;
    
  if (free )
    for (i =0; i <free ; i++)
      cogstop(cog[i]);
      
  return( free); 
}



/*
 *  check_input(fdserial *term)
 *
 *  Checks if any characters have been recived and move then to the buffer
 *  when a complete line has been recived copy the buffer to in_buf and 
 *  sets a flag to indicate a complete command has been recived.
 *  Uses:
 *    rx_count,rx_buf, 
 */
int check_input(fdserial *term)
{
  int whole_line=0;
  if (fdserial_rxReady(term) != 0) { // Non blocking check for data in the input buffer        
    while (rx_count < RXBUFFERLEN && (fdserial_rxReady(term) != 0)) {
      rx_buf[rx_count] = fdserial_rxTime(term, 10); // fdserial_rxTime will time out. Otherwise a spurious character on the line will cause us to get stuck forever      
      if (rx_buf[rx_count] == '\r' || rx_buf[rx_count] == '\n')
      {
        rx_buf[rx_count] = 0;
        memcpy(in_buf,rx_buf,rx_count+1);
        rx_count = 0;
        whole_line = 1;
        break;
      }       
      rx_count++;
    }          
  }
  return(whole_line);
}


/*
 *  clearTwistRequest()
 *  
 *  Reset the twist velocitys to 0
 *  Sets:
 *     CommandedVelocity, CommandedAngularVelocity, angularVelocityOffset
 */
void clearTwistRequest() {
  CommandedVelocity = 0.0;
  CommandedAngularVelocity = 0.0;
  angularVelocityOffset = 0.0;
}




/*
 *  pars_input()
 *
 *  Parse the data recived in the in_buf and populate 
 *  values as required
 *  Uses:
 *     CommandedVelocity, CommandedAngularVelocity, trackWidth, abd_speedLimit,distancePerCount, abdR_speedLimit
 *  Sets:
 *    expectedLeftSpeed, expectedRightSpeed, robotInitialized
 *    ignoreProximity, ignoreCliffSensors, ignoreIRSensors, ignoreFloorSensors, pluggedIn
 */
void pars_input()
{
  
  //const 
  char delimiter[2] = ","; // Delimiter character for incoming messages from the ROS Python script

  if (in_buf[0] == 's') 
  {
    char *token;
    token = strtok(in_buf, delimiter);
    token = strtok(NULL, delimiter);
    char *unconverted;
    CommandedVelocity = strtod(token, &unconverted);
    token = strtok(NULL, delimiter);
    CommandedAngularVelocity = strtod(token, &unconverted);

    angularVelocityOffset = CommandedAngularVelocity * (trackWidth * 0.5);
    /* Prevent saturation at max wheel speed when a compound command is sent.
       Without this, if your max speed is 50, and ROS asks us to set
       one wheel at 50 and the other at 100, we will end up with both
       at 50 changing a turn into a straight line!

       Remember that max speed is variable based on parameters within
       this code, such as proximity to walls, etc. */

    // Use forward speed limit for rotate in place.
    if (CommandedVelocity > 0 && (abd_speedLimit * distancePerCount) - fabs(angularVelocityOffset) < CommandedVelocity) 
    {
      CommandedVelocity = (abd_speedLimit * distancePerCount) - fabs(angularVelocityOffset);
      // Use abdR_speedLimit for reverse movement.
    } 
    else if (CommandedVelocity < 0 && -((abdR_speedLimit * distancePerCount) - fabs(angularVelocityOffset)) > CommandedVelocity) 
    {
      // In theory ROS never requests a negative angular velocity, only teleop
      CommandedVelocity = -((abdR_speedLimit * distancePerCount) - fabs(angularVelocityOffset));
    } 

  }     
   
  else if (in_buf[0] == 'd') 
  {
    char *token;
    token = strtok(in_buf, delimiter);
    token = strtok(NULL, delimiter);
    char *unconverted;
    trackWidth = strtod(token, &unconverted);
    token = strtok(NULL, delimiter);
    distancePerCount = strtod(token, &unconverted);
    token = strtok(NULL, delimiter);
    ignoreProximity = (int)(strtod(token, &unconverted));
    token = strtok(NULL, delimiter);
    ignoreCliffSensors = (int)(strtod(token, &unconverted));
    token = strtok(NULL, delimiter);
    ignoreIRSensors = (int)(strtod(token, &unconverted));
    token = strtok(NULL, delimiter);
    ignoreFloorSensors = (int)(strtod(token, &unconverted));
    token = strtok(NULL, delimiter);
    pluggedIn = (int)(strtod(token, &unconverted));
 
    #ifdef debugModeOn
    // For Debugging
    dprint(term, "GOT D! %.3f %.3f %d %d %d %d %d\n",trackWidth,distancePerCount, ignoreProximity, ignoreCliffSensors, ignoreIRSensors, ignoreFloorSensors, pluggedIn); 
    #endif

    //Additional included in init message but not reconfigure
    if(!robotInitialized)
    {
      token = strtok(NULL, delimiter);
      // Set initial location from ROS, in case we want to recover our last location!
      X = strtod(token, &unconverted);
      token = strtok(NULL, delimiter);
      Y = strtod(token, &unconverted);
      token = strtok(NULL, delimiter);
      Heading = strtod(token, &unconverted);
      #ifdef hasGyro
      gyroHeading = Heading;
      #endif
      if (trackWidth > 0.0 && distancePerCount > 0.0){
        robotInitialized = 1;
        #ifdef debugModeOn
        dprint(term, "Initalized \n");
        #endif
      }
    }               
  }   
}  





/*
 *  The Main Event
 *  
 */
int main()
{
  int tm = 0;
  int loop_time = 0;
  int state=0;
 
  simpleterm_close();
  term = fdserial_open(31, 30, 0, 115200);
  pause(1000);//Give the terminal a sec to get started

  //We rolled our own ms time cog  
  sequencer_start();
  
  
  #ifdef EMULATE_ROS
  pingArray[0] = 23;
  pingArray[1] = 23;
  pingArray[2] = 233;
  pingArray[3] = 24;
  pingArray[4] = 203;
  pingArray[5] = 23;
  irArray[0] = 22;
  srand(345);
  dprint(term, "Starting\n");  
  cogstart(&ROS, NULL, rstack, sizeof(rstack));
  #endif
  



  rx_count = 0; // clear the input buffer  

  // setup a buffer to build the output strings in
  char sensorbuf[132];
  memset(sensorbuf, 0, 132);
  char *curbuf = sensorbuf;  

  void handle_error()
  {
//    int e = get_last_error(sensorbuf);

    //get_last(sensorbuf);
    //dprint(term, "Sent %s" ,sensorbuf ); 
    //dprint(term, "DBH-10 Communications Error\n"); 
    //get_reply(sensorbuf);
    //dprint(term, "got \"%s\"\n" ,sensorbuf ); 

    //drive_get_ver(sensorbuf);
    dprint(term, "%s\n" ,sensorbuf ); 

    dbh_Error = 1;
    while(1) {pause(1000);}

  }    



       
  //used for calculating values     
  double deltaDistance = 0;
  double V = 0;
  double Omega = 0;
 
  //The requested speed 
  double expectedLeftSpeed = 0;
  double expectedRightSpeed = 0;
     
  //track the current speed settings
  int curLeftspeed = 0;
  int curRightSpeed =0;

  //Track the closet Ping and Ir sensor reading       
  int minPingDist;
  int minPingnum = 0;
  int minIrDist;
  int minIrnum;
       
       
       
/////////////////////////////////////////////
//Copied from ROSInterfaceForArloBotWithDHB10
  
  // Robot description: We will get this from ROS so that it is easier to tweak between runs without reloading the Propeller EEPROM.
  // http://learn.parallax.com/activitybot/calculating-angles-rotation
  // See ~/catkin_ws/src/ArloBot/src/arlobot/arlobot_bringup/param/arlobot.yaml to set or change this value

  distancePerCount = 0.0; 
  trackWidth = 0.0;

  // For Odometry
  int ticksLeft, ticksRight, ticksLeftOld=0, ticksRightOld=0;
  int speedLeft, speedRight, deltaX, deltaY, deltaTicksLeft, deltaTicksRight;
  int throttleStatus = 0;
  int heading;
  BatteryVolts = 12; //Just set it to something sane for right now

  /* Wait for ROS to give us the robot parameters,
     broadcasting 'i' until it does to tell ROS that we
     are ready */     
//  int robotInitialized = 0; // Do not compute odometry until we have the trackWidth and distancePerCount

  // For PIRsensor
  #ifdef hasPIR
  int PIRhitCounter = 0;
  int personThreshhold = 15; // Must get more than this number of hits before we call it a person.
  int personDetected = 0;
  #endif
  
  // For DHB-10 Interaction See drive_speed.c for example code
  // NOTE: Because this function has a loop, ALL interaction with the DHB-10 is done in this main loop.
  // Any other cog/function that needs to affect the robot's motors will set variables that are read in this function.

  drive_open();
//  pause(5);
  
/*
  drive_get_hwver(sensorbuf);
  dprint(term, "%s\n" ,sensorbuf ); 
    get_reply(sensorbuf);
    dprint(term, "* %s\n" ,sensorbuf ); 

  drive_get_ver(sensorbuf);
  dprint(term, "%s\n" ,sensorbuf ); 
    get_reply(sensorbuf);
    dprint(term, "* %s\n" ,sensorbuf ); 
*/  
//  dhb10_send("VER\r");
//  dhb10_send("VER\r");
//  // Halt motors in case they are moving and reset all stats.
//  if (drive_set_stop() )
//  {
//    handle_error();// handle error
//  }   
  
//  if ( drive_rst() )
//  {
//    handle_error();// handle error
//  }      
  // For Debugging without ROS:
  // See ~/catkin_ws/src/ArloBot/src/arlobot/arlobot_bringup/param/arlobot.yaml for most up to date values
  /*  
  trackWidth = 0.403000; // from measurement and then testing
  distancePerCount = 0.00338;
  // http://forums.parallax.com/showthread.php/154274-The-quot-Artist-quot-robot?p=1271544&viewfull=1#post1271544
  robotInitialized = 1;
  */  
  // Comment out above lines for use with ROS

  // Declaring variables outside of loop
  // This may or may not improve performance
  // Some of these we want to hold and use later too
  // A Buffer long enough to hold the longest line ROS may send.
  //const int bufferLength = 35; // A Buffer long enough to hold the longest line ROS may send.
  //char buf[bufferLength];
  //int count = 0;
  int i = 0;
    
  // To hold received commands
  CommandedVelocity = 0.0;
  CommandedAngularVelocity = 0.0;
  angularVelocityOffset = 0.0; 

  expectedLeftSpeed = 0; 
  expectedRightSpeed = 0;
  curLeftspeed = 0; 
  curRightSpeed =0;    
    
  // Listen for drive commands
  int timeoutCounter = 0;
        
//End of init from ROSInterfaceForArloBotWithDHB10  
//////////////////////////////////////////////////
//dprint(term, "Starting loop\n"); 
 
  while(1)
  {    
  
     timeoutCounter++;//keep track of timoutcount
     
     got_one |= check_input(term);//We or = to prevent clearing 
     
     if(got_one){
      #ifdef debugModeOn 
      //dprint(term,"%s",in_buf);
      #endif
      pars_input(); //5ms

      #ifdef debugModeOn 
      //dprint(term,"C=%.3f A=%.3f \t",CommandedVelocity,angularVelocityOffset);
      #endif

      // These only need to be calculated once per twist msg
      expectedLeftSpeed =  CommandedVelocity - angularVelocityOffset;
      expectedRightSpeed = CommandedVelocity + angularVelocityOffset;
      #ifdef debugModeOn 
      //dprint(term,"L=%f R=%f\t",expectedLeftSpeed,expectedRightSpeed);
      #endif
      
      expectedLeftSpeed =  expectedLeftSpeed / distancePerCount;
      expectedRightSpeed = expectedRightSpeed / distancePerCount;                
      #ifdef debugModeOn 
      //dprint(term,"l=%d r=%d speed l=%d r=%d\n",(int)expectedLeftSpeed,(int)expectedRightSpeed,curLeftspeed,curRightSpeed);
      #endif

      //Clear flag and timeout      
      got_one = 0;
      timeoutCounter = 0;
    }// Timout code needs to be though out better    
    if (timeoutCounter > ROStimeout) 
    {
        #ifdef debugModeOn
        dprint(term, "DEBUG: Stopping Robot due to serial timeout.\n");
        #endif
        expectedLeftSpeed = 0;
        expectedRightSpeed = 0;
        //clearTwistRequest();
        timeoutCounter = 0; //ROStimeout; // Prevent runaway integer length          
    }     
    
      
    /* This updates the motor controller on EVERY
       round. This way even if there is no updated twist command
       from ROS, we will still account for updates in the speed limit
       from the SaftyOverride cog by recalculating the drive commands
       based on the new speed limit at every loop.

       This also allows us to have a STOP action if there is no input
       from ROS for too long.
    */
/*
    if ( safty_check(CommandedVelocity,&expectedLeftSpeed,&expectedRightSpeed) )
    {
      #ifdef debugModeOn
      dprint(term, "Safty_Check l=%f r=%f\n",expectedLeftSpeed,expectedRightSpeed );  
      #endif
      //clearTwistRequest();//ignore twist msg if we are escaping or blocked
    }    
*/              
    /* to simplify communications with teh dhb-10 we send the command to go in only one place */
    /* first check if there has been a change dont overload with needless commands */
    if( (curLeftspeed != (int)expectedLeftSpeed || curRightSpeed != (int)expectedRightSpeed) && robotInitialized )
    {
      curLeftspeed = (int)expectedLeftSpeed;
      curRightSpeed = (int)expectedRightSpeed;
      //pause(dhb10OverloadPause);
      if ( drive_set_gospd(curLeftspeed,curRightSpeed) )
      {
        handle_error();// handle error
      }    
      #ifdef debugModeOn
      dprint(term, "go speed l=%d r=%d\n",curLeftspeed,curRightSpeed );
      #endif
    }        


    // Broadcast Odometry  
    /* Some of the code below came from Dr. Rainer Hessmer's robot.pde
       The rest was heavily inspired/copied from here:
       http://forums.parallax.com/showthread.php/154963-measuring-speed-of-the-ActivityBot?p=1260800&viewfull=1#post1260800
    */

    switch(state)
    {
      
      case 0: //Ros has not initalized the bot yet
      
        if(!robotInitialized)
        {
          state = -1; //We increment after switch putting us back to 0
          throttleStatus++;
          if(throttleStatus > 30)
          {
            // Request Robot distancePerCount and trackWidth 
            //NOTE: Python code cannot deal with a line with no divider characters on it.
            #ifdef hasPIR
            dprint(term, "i\t%d\n", personDetected);
            #else
            dprint(term, "i\t0\n");
            #endif 
            
          
            //Copied from origional code, should be reworked.
            #ifdef hasPIR
            int PIRstate = 0;
            for (i = 0; i < 5; i++) // 5 x 200ms pause = 1000 between updates
            {
              PIRstate = input(PIR_PIN); // Check sensor (1) motion, (0) no motion
              // Count positive hits and make a call:
              if (PIRstate == 0) 
              {
                PIRhitCounter = 0;
              } 
              else 
              {
              PIRhitCounter++; // Increment on each positive hit
              }
              if (PIRhitCounter > personThreshhold) 
              {
                personDetected = 1;
              } 
              else 
              {
                personDetected = 0;
              }
              pause(200); // Pause 1/5 second before repeat
            }
            #endif 
            throttleStatus=0;         
          }
        }                 
        else
        {//Start ping code etc.
          #ifdef debugModeOn
          dprint(term, "Starting Cogs\n");
          #endif
          
          #ifndef EMULATE_ARLO
          // Start the local sensor polling cog
          ping_start();
          #endif

          // Start Gyro polling in another cog
          #ifdef hasGyro
            gyro_start();
          #endif

          // Start safetyOverride cog: (AFTER the Motors are initialized!)
          #ifndef EMULATE_ARLO
          safetyOverride_start();     
          #endif
          timeoutCounter = 0;//clear timeout counter  
           
        }          
        break;
        
      case 1:// read speed   8ms        
        if ( drive_get_spd(&speedLeft, &speedRight) )
        {
          handle_error();// handle error
        }    
        break;

      case 2:// read distance  8ms
        ticksLeftOld = ticksLeft;
        ticksRightOld = ticksRight;
        if ( drive_get_dist(&ticksLeft,&ticksRight) )
        {
          handle_error();// handle error
        }          
        break;

      case 3:// read heading  7ms
        if ( drive_get_head(&heading) )
        {
          handle_error();// handle error
        }            
        // The heading is apparently reversed in relation to what ROS expects, hence the "-heading"
        Heading = -heading * PI / 180.0; // Convert to Radians

        deltaTicksLeft = ticksLeft - ticksLeftOld;
        deltaTicksRight = ticksRight - ticksRightOld;
        deltaDistance = 0.5f * (double) (deltaTicksLeft + deltaTicksRight) * distancePerCount;
        deltaX = deltaDistance * (double) cos(Heading);
        deltaY = deltaDistance * (double) sin(Heading);

        X += deltaX;
        Y += deltaY;

        // http://webdelcire.com/wordpress/archives/527
        V = ((speedRight * distancePerCount) + (speedLeft * distancePerCount)) / 2;
        Omega = ((speedRight * distancePerCount) - (speedLeft * distancePerCount)) / trackWidth;
    
        break;

      case 4:// format odometery  13ms
        // Odometry for ROS
        /*
           I sending ALL of the proximity data (IR and PING sensors) to ROS
           over the "odometry" line, since it is real time data which is just as important
           as the odometry, and it seems like it would be faster to send and deal with one packet
           per cycle rather than two.

           In the propeller node I will convert this to fake laser data.
           I have two goals here:
           1. I want to be able to visualize in RVIZ what the sensors are reporting. This will help with debugging
           situations where the robot gets stalled in doorways and such due to odd sensor readings from angled
           surfaces near the sides.
           2. I also want to use at least some of this for obstacle avoidance in AMCL.
           Note that I do not think that IR and PING data will be useful for gmapping, although it is possible.
           It is just too granular and non specific. It would be nice to be able to use the PING (UltraSonic) data
           to deal with mirrors and targets below the Kinect/Xtion, but I'm not sure how practical that is.
        */
        memset(sensorbuf, 0, 132);
        #ifdef hasGyro
        sprint(sensorbuf,"o\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f",X,Y,Heading,gyroHeading,V,Omega);
        #else      
        //sprint(sensorbuf,"o\t%.3f\t%.3f\t%.3f\t0.0\t%.3f\t%.3f",X,Y,Heading,V,Omega);
        sprint(sensorbuf,"o\t%d\t%d\t%.3f\t0.0\t%.3f\t%.3f",deltaTicksLeft,deltaTicksRight,Heading,V,Omega);
        #endif
        curbuf = sensorbuf + strlen(sensorbuf);
        break;

      case 5:// format ping       8ms
        sprint(curbuf,"\t{");
        curbuf = sensorbuf + strlen(sensorbuf);
        minPingDist = 255;
        minPingnum = 255;
        minIrDist = 255;
        minIrnum = 255;
        for (i = 0; i < NUMBER_OF_PING_SENSORS; i++) { // Loop through all of the sensors
          if (i > 0)
          {
           sprint(curbuf, ",");
           curbuf = sensorbuf + strlen(sensorbuf);
          }                
          sprint(curbuf, "\"p%d\":%d ", i, pingArray[i]);
          curbuf = sensorbuf + strlen(sensorbuf);
          if(pingArray[i] < minPingDist)
          {
            minPingDist = pingArray[i];
            minPingnum = i;
          }            
        }      
        for (i = 0; i < NUMBER_OF_IR_SENSORS; i++) // Loop through all of the sensors
        {
          if (irArray[i] > 0) // Don't pass the empty IR entries, as we know there are some.
            sprint(curbuf, ",\"i%d\":%d", i, irArray[i]);
            curbuf = sensorbuf + strlen(sensorbuf);
            if(irArray[i] < minIrDist)
            {
              minIrDist = irArray[i];
              minIrnum = i;
            }            
        }
        
        #ifdef hasFloorObstacleSensors
        for (i = 0; i < NUMBER_OF_FLOOR_SENSORS; i++) // Loop through all of the sensors
        {
          sprint(curbuf, ",\"f%d\":%d", i, floorArray[i]);
          curbuf = sensorbuf + strlen(sensorbuf);
        }
        #endif
        sprint(curbuf, "}\n");
        break;

      case 6:// xmit odometry     10ms
        #ifdef enableOutput
        dprint(term,sensorbuf); // 10 ms
        #endif 
        throttleStatus = throttleStatus + 1;
        if(throttleStatus < 10)
           state = 0;//We increment after switch
        break;

      case 7:// xmit status       16ms
        // Send a regular "status" update to ROS including information that does not need to be refreshed as often as the odometry.
        dprint(term, "s\t%d\t%d\t%d\t%d\t%d\t%d\t%.2f\t%.2f\t%d\t%d\n", 
                  safeToProceed, safeToRecede, Escaping, abd_speedLimit, 
                  abdR_speedLimit, minPingnum, 
                  BatteryVolts, RawBatVolts, 
                  cliff, pingArray[minPingnum] ); //floorO);
                  minPingnum = minIrnum;//hush the compiler warning
        throttleStatus = 0;
        state = 0; //We increment after switch
//        tm +=  sequencer_get(); // ~1ms or less    
//        dprint(term,"%d \n ",tm);
//        tm=0;
        break;
    } 
    
           
    state++;
    while(sequencer_get() < 16 ) {;} //16 = 60 hz

//dprint(term,"%d %d\n",loop_time,state);

//tm = sequencer_get(); // ~1ms or less   
loop_time =  sequencer_get();
tm += loop_time;
sequencer_reset(); 

  
#ifdef EMULATE_ROS 
 ros_t = rand() % 200 + 0;
 ros_r=ros_t/100;
      
 ros_t = rand() % 100 + 1;
 if(ros_t > 90)
 {
  ros_t = rand() % 100 + 1;
  ros_v=ros_t/100;
 }        
 else
 {
  ros_v=0;       
 } 
             
#endif 




 }
 return(0); 
}  


/*
 *twist message
 *message = 's,%.3f,%.3f\r' % (v, omega)
 *'s,2.000,0.000r'
 *
 *
 *startup message
 *message = 'd,%f,%f,%d,%d,%d,%d,%d,%f,%f,%f\r' % 
             (self.track_width, self.distance_per_count, ignore_proximity, 
              ignore_cliff_sensors, ignore_ir_sensors, ignore_floor_sensors, 
               ac_power, self.lastX, self.lastY, self.lastHeading)
 *'d,0.0,0.0,1,1,1,1,1,0.0,0.0,0.0r' 
 *
 *ReConfiguer
 *message = 'd,%f,%f,%d,%d,%d,%d,%d\r' % 
             (self.track_width, self.distance_per_count, ignore_proximity, 
              ignore_cliff_sensors, ignore_ir_sensors, ignore_floor_sensors,
              ac_power)
 *
 *
 */
#ifdef EMULATE_ROS
void ROS()
{
 //pretend we are ROS and stuff the buffer 
 int throttle=0;
 //int t=0;
 //char tmp[RXBUFFERLEN];
 
 pause(3000); 
 strcpy(in_buf,"d,0.403000,0.00338,0,1,1,1,0,0.0,0.0,0.0\r");// A Buffer long enough to hold the longest line ROS may send.
 got_one = 1; 
 while(got_one){;}//Spin till the robot ready
 double r=0.0;
 double v=0.9;
 int ping;
 pause(1000);
//#define FRONT_CENTER_SENSOR 4
//#define FRONT_CENTER_HALT_DIST 10
//#define FRONT_CENTER_SLOWDWN_DIST 20 
 while(1)
 {
    pause(100);
    ping = pingArray[FRONT_CENTER_SENSOR];
    //ping = 45;
    if(ping < 30)
    {
      r = -0.15;
      v = 0.1;
    }
    else if( ping > 40)
    {
      r = 0.18;
      v = 0.1;
    }
    else
    {
      r = 0.0;
      v = 0.0;
    }            
            
    
/*  random moving
    throttle++;
    if (throttle > 25)
    {
      r=ros_r;
      v=ros_v;
      throttle=0;
    }      
*/
    sprint(in_buf,"s,%.3f,%.3f\r",r,v);
    got_one = 1;
 }    
}  
#endif






