1:
cd command does not work in the terminal.

    a) When the direcoty exists, nothing shows up
    b) When the directory does not exist, it breaks the terminal.
    --FIXED--


2:
Pinecode API key does not work.
--BROKEN--

3:
add a way to go back to the previous directory.
--FIXED--

4:
The AI has issues with running code in the terminal.


   a) When you ask the AI to make a next JS app, it does not work.
      i) The issue is that the AI does not run the command and wait for the result and then put another request to GROQ api to do the next command.
   --BROKEN--
   b) The AI has issues running pyton scripts.
      i)The issue is that the AI runs python and not python3 for the command.
   --BROKEN--

5:
When the AI runs code, it does not wait for the code to ask a question, but it answers the question before the code asks it.

   a) The AI should do one command at a time. Have the AI wait for the code to run if requred, and let it run if the code it done. and there should be a different request for the next command to groq. This is unless the code can be run without waiting.
   --BROKEN--