this is a fixed and updated version of raop_play
for newer airport express like this one :

http://www.apple.com/airport-express/

apart for fixing the timing protocol,
which respects now the spec of airport 2 protocol,
the main trick was to send non-encrypted 
audio frames, because encryption 
was failing with newer airport express.


* to compile it :

make clean
make 
make install

* to use it :

raop_play -e -p 5000 <airport ip> <wav file>

( the -e is essential here as it means 'non-encrypted' )

it only supports wav format for now.


fixed and forked by sevy : ydegoyon@gmail.com
