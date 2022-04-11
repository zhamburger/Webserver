# Web Servers and Synchronization

Multi-Threaded server which can handles requests using 1 of 4 different scheduling policies to synchronize threads, logging usage statistics of server requests.
Multi-Threaded Client which sends requests to the server in a pre-specified convention (First-In-First-Out or Concurrently)

Written using C and pthread libraries 

# Project Partners:  
    Zach Hamburger     
    Nathaniel Silverman
    
# Server Side

   Our code is split into three different main structures. 
   1) A thread structure that holds a thread and its id, thread count, HTML count, and image count. 
   2) A connection structure that holds the port it's supposed to connect to, all the times and counts statistics it's supposed to hold, and the file it's going to execute. 
   3) A queue structure that holds an array of connections, pointers to the head and tail, and the max capacity of the server BUFFER. 

   When executing the server it is passed in the port it's running on, the directory structure, the number of thread workers, the max capacity of the BUFFER, and the scheduling policy indicating how requests are to be handled. Once the server is up and running it creates a thread pool based on the number of threads passed in. On creation of each thread, they are passed in one of two routines. Then based on the scheduling policy, the server creates one or two queues. When the scheduling policy is "FIFO" or "ANY" it instantiates only one queue that has no bias on what kind of file is being requested. However, if the scheduling policy is "HPIC" or "HPHC" it instantiates two queues, one queue for higher prioritized content types and a second queue for the non-prioritized content types. The server then starts listening and accepting connections from client requests.

  Once a connection gets accepted the server pre-processes its file type. If the scheduling policy being used is "HPIC"/"HPHC" its file type is used to put it into the appropriate queue based on which High-Priority scheduling policy is set. If the scheduling policy is "FIFO"/"ANY" put into the single queue. If any worker threads are free, it wakes up and executes the routine it was assigned. The routine first figures out which of the two routines it's supposed to run. If the scheduling policy is "FIFO" it locks the mutex so it can access the queue, if the queue is empty the thread waits to be signaled by a condition variable. It then dequeues a connection and executes it, after which it unlocks the mutex for the next worker to access the queue. If the scheduling policy is "HPHC" or "HPIC" it locks the mutex, if both of the queues are empty then the worker thread waits until it is signaled by the condition variable. It first tries to access the higher prioritized queue, if that queue is empty it goes to the other one, once it dequeues a connection it executes it, and unlocks the mutex, signaling the main thread that the there is an available spot in the queue.

   A thread keeps track of all it does in a lifetime. From when it accepts requests to when it executes them, and the number of different file types it executes. It also consults the server about how many requests were executed before it, how many cut it in line, and the times of its execution relative to the creation of the server. All this communication is done within the mutex lock so it can access this information such that it is thread-safe.

# Client Side 

   When a client gets instantiated it creates a thread pool so it can send multiple requests at a time to the server. Each thread is given a routine based on the policy it was given. If the policy is "CONCUR" it locks a mutex then connects to the port then unlocks the mutex then sends the request to the server, so they are all running concurrently after they connect. Once a worker thread finishes executing, it stops and waits at a barrier until all the other threads finish completing their task, at which point the barrier goes down and all the worker threads can send more requests. If the policy is FIFO it uses a semaphore lock ensuring that the connections given to the port are made sequentially. If the client was given two files, all the threads run the first file then they wait by a barrier to swap to file two.  

# Ambiguities 
  
  * We preprocess the request calling read to find file type beforehand. 
  * For the scheduling policy "ANY" we use FIFO so that the requests enter the queue sequentially but are executed concurrently. 

# Known Bugs/Issues
   
   * When it came to bugs we found it most helpful to utilize the logger to log all the statistics and errors that we encountered. 
   * A few bugs that we found were due to our code, but some others were due to being slightly unfamiliar with the ports and connection.
   *  When our worker threads would lock the mutex the server would seem to get stuck, after reading more about the mutexes we realized we accidentally tried to lock a mutex while the mutex was locked.
   *  Once we got the client to execute it would close the server after executing the given file, we found that the server was first intended to accept one request and fork it as a process, so we just had to stop the client from closing that specific proccess. 
   *  Then once we were manipulating the scheduling policy we had an issue with pre-processing the file type then executing it again. After a day of researching it seemed like whenever the given file was read we had to save that into a buffer for later execution, so we added that function inside of the connection structure. 
   *  Another weird bug was running the server on the same port right after it was previously ran on that same port running other would cause an error that the port was in use already, during testing we had to switch the port number to run server on.


# Testing

   * When testing FIFO we would run the server with two files, HTML files, and JPG files. We then observed that the server was executing all HTML files then all JPG files, which indicated to us that it would run each file in the given order from the client because the client would send all of single type at a time.
   * When testing HPHC and HPIC we would run the client with double the thread workers than the server and two different kinds of files. We then observed that HPHC was starving all the images passed in while HPIC was starving all the HTML files passed in.
   * We tested the client's scheduling policy "FIFO" by running a server with a High-Priority scheduling policy, which we noticed that the server didn't fully starve the lower prioritized content type because it was sending requests sequentially.
      
    
# Running the Server Examples

    Show that the server is correctly running:
    1) FIFO policy
        * ./server 5003 . 8 16 FIFO &
        * ./client localhost 5003 8 CONCUR /index.html /nigel.jpg
        * Note: it runs 8 HTMLs then 8 images 
    2) HPIC policy
        * ./server 5069 . 4 30 HPIC &
        * ./client localhost 5069 8 CONCUR /index.html /nigel.jpg
        * Note: it takes some time for starvation of HTML requests
    3) HPHC policy
        * ./server 5016 . 4 30 HPHC &
        * ./client localhost 5016 8 CONCUR /nigel.jpg /index.html
        * Note: it takes some time for starvation of image requests
        
    Remember to run "ps aux | grep server 5003" then "kill" the server proccess before running again
    
# Sample Output
    HTTP/1.1 200 OK
    Server: nweb/23.0
    Content-Length: 248
    Connection: close
    Content-Type: text/html

    X-stat-req-arrival-count: 0
    X-stat-req-arrival-time: 227735
    X-stat-req-dispatch-count: 0
    X-stat-req-dispatch-time: 228257
    X-stat-req-complete-count: 0
    X-stat-req-complete-time: 228372
    X-stat-req-age: 0
    X-stat-thread-id: 7
    X-stat-thread-count: 1
    X-stat-thread-html: 1
    X-stat-thread-image: 0
    <HTML>
    <TITLE>nweb
    </TITLE>
    <BODY BGCOLOR="lightblue">
    <H1>nweb Test page</H1>
    <!-- <IMG SRC="nigel.jpg"> -->
    <p>
    Not pretty but it should prove that nweb works :-)
    <p>
    Feedback is welcome to Nigel Griffiths nag@uk.ibm.com
    </table>
    </BODY>
    </HTML>
