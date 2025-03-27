#include<stdio.h>
#include<unistd.h>
#include<fcntl.h>
#include<stdlib.h>


const char* pathname="/sys/bus/nvmem/devices/rockchip-efuse0/nvmem";
const char* newfile="efusedump.txt";
#define SIZE 128

int main(){
     
     int fd,newfd;
     FILE* fptr=NULL;
     int i=0,iCnt=0;
     char buffer[SIZE];
     char binbuffer[SIZE];
     fd=open(pathname,O_RDONLY);
     if(fd==-1){
          perror("Couldn't open file \n");
          return -1;
     }
          
    

     iCnt=read(fd,buffer,SIZE);

     if(iCnt<=0){
          perror("read file failed\n");
          exit(-1);
     }    

     newfd=creat(newfile,0770);
     if(newfd ==-1){
          perror("Couldn't create new file dump\n");
          exit(-1);
     }

     iCnt=write(newfd,buffer,sizeof(buffer));

     printf("%d bytes written \n",iCnt);

    // iCnt=write(STDOUT_FILENO,buffer,sizeof(buffer));
     fptr=fopen(newfile,"r");
     if(fptr==NULL){
          perror("couldn't open file as a stream\n");
          exit(EXIT_FAILURE);
     }

     
     size_t f_bytes_read=fread(binbuffer,1,sizeof(buffer),fptr);
     
     printf("%lu bytes read \n",f_bytes_read);

     FILE* fdump=fopen("efuse_bin_dump.txt","w");

     size_t f_bytes_written=fwrite(binbuffer,1,f_bytes_read,fdump);

     printf("%lu bytes written \n",f_bytes_written);

     for(i=0;i<sizeof(binbuffer);i++){
          if(i%4==0){
	      	printf("\n 0x%08x \n",(unsigned int)binbuffer[i]);
	      } 
          if(binbuffer[i]=='\0'){
               printf("\t0x00");
          }
          else{
               printf("\t%c",binbuffer[i]);
	     
          }
          if(i%2!=0 && i!=0){
               puts("\n");
          }
     }
     fclose(fdump);
     fclose(fptr);
     close(newfd);
     close(fd);

     return 0;
}

