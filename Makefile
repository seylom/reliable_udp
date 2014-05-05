all: reliable_sender reliable_receiver

reliable_sender: MP3-sender.c helper.h
	gcc -g -pthread -w -o reliable_sender MP3-sender.c

reliable_receiver: MP3-receiver.c helper.h
	gcc -g -pthread -w -o reliable_receiver MP3-receiver.c

clean:
	rm -rf *o reliable_sender reliable_receiver
