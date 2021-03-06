/*------------------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.


Original source: https://blog.csdn.net/luxiaoxun/article/details/7622988

1. Modified for a 240x320 SPI LCD display.
2. The width of the displaying picture must be a multiple of 4.
3. Applay show_jpg() or show_bmp() in main().

TODO:
    1. to pad width to a multiple of 4 for bmp file.
    2. jpgshow() picture flips --OK
    3. in show_jpg(), just force 8bit color data to be 24bit, need to improve.!!!


./open-gcc -L./lib -I./include -ljpeg -o jpgshow fbshow.c

Modified by Midas Zhou
-----------------------------------------------------------------------*/
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <linux/fb.h> //u
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <jpeglib.h>
#include <jerror.h>
#include <dirent.h>
#include "egi_image.h"
#include "egi_debug.h"
#include "egi_bmpjpg.h"
#include "egi_color.h"
#include "egi_timer.h"

//static BITMAPFILEHEADER FileHead;
//static BITMAPINFOHEADER InfoHead;

/*--------------------------------------------------------------
 open jpg file and return decompressed image buffer pointer
 int *w,*h:   with and height of the image
 int *components:  out color components
 return:
	=NULL fail
	>0 decompressed image buffer pointer
--------------------------------------------------------------*/
unsigned char * open_jpgImg(char * filename, int *w, int *h, int *components, FILE **fil)
{
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
        FILE *infile;
        unsigned char *buffer;
        unsigned char *temp;

        if (( infile = fopen(filename, "rb")) == NULL) {
                fprintf(stderr, "open %s failed\n", filename);
                return NULL;
        }

	/* return FILE */
	*fil = infile;

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);

        jpeg_stdio_src(&cinfo, infile);

        jpeg_read_header(&cinfo, TRUE);

        jpeg_start_decompress(&cinfo);
        *w = cinfo.output_width;
        *h = cinfo.output_height;
	*components=cinfo.out_color_components;

//	printf("output color components=%d\n",cinfo.out_color_components);
//        printf("output_width====%d\n",cinfo.output_width);
//        printf("output_height====%d\n",cinfo.output_height);

	/* --- check size ----*/
/*
        if ((cinfo.output_width > 240) ||
                   (cinfo.output_height > 320)) {
                printf("too large size JPEG file,cannot display\n");
                return NULL;
        }
*/

        buffer = (unsigned char *) malloc(cinfo.output_width *
                        cinfo.output_components * cinfo.output_height);
        temp = buffer;

        while (cinfo.output_scanline < cinfo.output_height) {
                jpeg_read_scanlines(&cinfo, &buffer, 1);
                buffer += cinfo.output_width * cinfo.output_components;
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);

        return temp;

        fclose(infile);
}

/*    release mem for decompressed jpeg image buffer */
void close_jpgImg(unsigned char *imgbuf)
{
	if(imgbuf != NULL)
		free(imgbuf);
}


/*------------------------------------------------------------------
open a 24bit BMP file and write image data to FB.
image size limit: 320x240

Note:
    1. Read color pixel one by one, is rather slower.
    2. For BGR type onley, BGRA is not supported. 

blackoff:   1	 Do NOT wirte black pixels to FB.
		 (keep original data in FB)

	    0	 Wrtie  black pixels to FB.
x0,y0:		start point in LCD coordinate system

Return:
	    1   file size too big
	    0	OK
	   <0   fails
--------------------------------------------------------------------*/
int show_bmp(char* fpath, FBDEV *fb_dev, int blackoff, int x0, int y0)
{
	BITMAPFILEHEADER FileHead;
	BITMAPINFOHEADER InfoHead;
	unsigned char *buff;
	int j,k;
	int pad; /*in byte, pad to multiple of 4bytes */

	FILE *fp;
	int xres=fb_dev->vinfo.xres;
	int bits_per_pixel=fb_dev->vinfo.bits_per_pixel;
	int rc;
//	int line_x, line_y;
	long int location = 0;
	uint16_t color;
	PIXEL pix;

	/* get fb map */
	unsigned char *fbp =fb_dev->map_fb;

	printf("fpath=%s\n",fpath);
	fp = fopen( fpath, "rb" );
	if (fp == NULL)
	{
		return -1;
	}

	rc = fread( &FileHead, sizeof(BITMAPFILEHEADER),1, fp );
	if ( rc != 1)
	{
		printf("Read FileHead error!\n");
		fclose( fp );
		return -2;
	}

	//检测是否是bmp图像
	if (memcmp(FileHead.cfType, "BM", 2) != 0)
	{
		printf("It's not a BMP file\n");
		fclose( fp );
		return -3;
	}

	rc = fread((char *)&InfoHead, sizeof(BITMAPINFOHEADER), 1, fp);
	if ( rc != 1)
	{
		printf("Read InfoHead error!\n");
		fclose( fp );
		return -4;
	}

	printf("BMP width=%d,height=%d  ciBitCount=%d\n",(int)InfoHead.ciWidth, (int)InfoHead.ciHeight, (int)InfoHead.ciBitCount);
	//检查是否24bit色
	if(InfoHead.ciBitCount != 24 ){
		printf("It's not 24bit_color BMP!\n");
		return -5;
	}

#if 0 /* check picture size */
	//检查宽度是否240x320
	if(InfoHead.ciWidth > 240 ){
		printf("The width is great than 240!\n");
		return -1;
	}
	if(InfoHead.ciHeight > 320 ){
		printf("The height is great than 320!\n");
		return -1;
	}

	if( InfoHead.ciHeight+y0 > 320 || InfoHead.ciWidth+x0 > 240 )
	{
		printf("The origin of picture (x0,y0) is too big for a 240x320 LCD.\n");
		return -1;
	}
#endif

//	line_x = line_y = 0;

	fseek(fp, FileHead.cfoffBits, SEEK_SET);

	/* if ciWidth_bitcount is not multiple of 4bytes=32, then we need skip padded 0,
	 * skip=0, if ciWidth_bitcount is multiple of 4bytes=32.
	 *  if line=7*3byte=21bytes, 21+3_pad=24yptes  skip=3bytes.
	 */
	pad=( 4-(((InfoHead.ciWidth*InfoHead.ciBitCount)>>3)&3) ) &3; /* bytes */
	printf("Pad byte for each line: %d \n",pad);

	/* calloc buff for one line image */
	buff=calloc(1, InfoHead.ciWidth*sizeof(PIXEL)+pad );
	if(buff==NULL) {
		printf("%s: Fail to alloc buff for color data.\n",__func__);
		return -6;
	}

	/* Read one line each time, read more to speed up process */
	for(j=0;j<InfoHead.ciHeight;j++)
	{
		/* Read one line with pad */
		rc=fread(buff,InfoHead.ciWidth*sizeof(PIXEL)+pad,1,fp);
		if( rc < 1 ) {
			printf("%s: fread error.%s\n",__func__,strerror(errno));
			return -7;
		}

		/* write to FB */
		for(k=0; k<InfoHead.ciWidth; k++) /* iterate pixel data in one line */
		{
			pix=*(PIXEL *)(buff+k*sizeof(PIXEL));

			location = k * bits_per_pixel / 8 +x0 +
					(InfoHead.ciHeight - j - 1 +y0) * xres * bits_per_pixel / 8;
	        	/* NOT necessary ???  check if no space left for a 16bit_pixel in FB mem */
        		if( location<0 || location>(fb_dev->screensize-bits_per_pixel/8) )
	        	{
               			 printf("show_bmp(): WARNING: point location out of fb mem.!\n");
				 free(buff);
  		                 return 1;
        		}
			/* converting to format R5G6B5(as for framebuffer) */
			color=COLOR_RGB_TO16BITS(pix.red,pix.green,pix.blue);
			/*if blockoff>0, don't write black to fb, so make it transparent to back color */
			if(  !blackoff || color )
			{
				*(uint16_t *)(fbp+location)=color;
			}
		}
	 }

	free(buff);
	fclose( fp );
	return( 0 );
}


/*-------------------------------------------------------------------------
open a BMP file and write image data to FB.
image size limit: 320x240

blackoff:   1   Do NOT wirte black pixels to FB.
		 (keep original data in FB,make black a transparent tunnel)
	    0	 Wrtie  black pixels to FB.
(x0,y0): 	original coordinate of picture in LCD

Return:
	    0	OK
	    <0  fails
-------------------------------------------------------------------------*/
int show_jpg(char* fpath, FBDEV *fb_dev, int blackoff, int x0, int y0)
{
	int xres=fb_dev->vinfo.xres;
	int bits_per_pixel=fb_dev->vinfo.bits_per_pixel;
	int width,height;
	int components; 
	unsigned char *imgbuf;
	unsigned char *dat;
	uint16_t color;
	long int location = 0;
	int line_x,line_y;

	FILE *infile=NULL;

	/* get fb map */
	unsigned char *fbp =fb_dev->map_fb;

	imgbuf=open_jpgImg(fpath,&width,&height,&components, &infile);

	if(imgbuf==NULL) {
		printf("open_jpgImg fails!\n");
		return -1;
	}

	/* WARNING: need to be improve here: converting 8bit to 24bit color*/
	if(components==1) /* 8bit color */
	{
		height=height/3; /* force to be 24bit pic, however shorten the height */
	}

	dat=imgbuf;


//       printf("open_jpgImg() succeed, width=%d, height=%d\n",width,height);
#if 0	//---- normal data sequence ------
	/* WARNING: blackoff not apply here */
	line_x = line_y = 0;
	for(line_y=0;line_y<height;line_y++) {
		for(line_x=0;line_x<width;line_x++) {
			location = (line_x+x0) * bits_per_pixel / 8 + (height - line_y - 1 +y0) * xres * bits_per_pixel / 8;
			//显示每一个像素, in ili9431 node of dts, color sequence is defined as 'bgr'(as for ili9431) .
			// little endian is noticed.
   	        	// ---- dat(R8G8B8) converting to format R5G6B5(as for framebuffer) -----
			color=COLOR_RGB_TO16BITS(*dat,*(dat+1),*(dat+2));
			/*if blockoff>0, don't write black to fb, so make it transparent to back color */
			if(  !blackoff || color )
			{
				*(uint16_t *)(fbp+location)=color;
			}
			dat+=3;
		}
	}
#else
	//---- flip picture to be same data sequence of BMP file ---
	line_x = line_y = 0;
	for(line_y=height-1;line_y>=0;line_y--) {
		for(line_x=0;line_x<width;line_x++) {
			location = (line_x+x0) * bits_per_pixel / 8 + (height - line_y - 1 +y0) * xres * bits_per_pixel / 8;
			//显示每一个像素, in ili9431 node of dts, color sequence is defined as 'bgr'(as for ili9431) .
			// little endian is noticed.
   	        	// ---- dat(R8G8B8) converting to format R5G6B5(as for framebuffer) -----
			color=COLOR_RGB_TO16BITS(*dat,*(dat+1),*(dat+2));
			/*if blockoff>0, don't write black to fb, so make it transparent to back color */
			if(  blackoff<=0 || color )
			{
				*(uint16_t *)(fbp+location)=color;
			}
			dat+=3;
		}
	}
#endif
	close_jpgImg(imgbuf);
	fclose(infile);
	return 0;
}


/*------------------------------------------------------------------------
allocate memory for egi_imgbuf, and then load a jpg image to it.

fpath:		jpg file path

//fb_dev:		if not NULL, then write to FB,

imgbuf:		buf to hold the image data, in 16bits color
		input:  an EGI_IMGBUF
		output: a pointer to the image data

Return
		0	OK
		<0	fails
-------------------------------------------------------------------------*/
int egi_imgbuf_loadjpg(char* fpath,  EGI_IMGBUF *egi_imgbuf)
{
	//int xres=fb_dev->vinfo.xres;
	//int bits_per_pixel=fb_dev->vinfo.bits_per_pixel;
	int width,height;
	int components;
	unsigned char *imgbuf;
	unsigned char *dat;
	uint16_t color;
	long int location = 0;
	int btypp=2; /* bytes per pixel */
	int i,j;
	FILE *infile=NULL;

	/* open jpg and get parameters */
	imgbuf=open_jpgImg(fpath,&width,&height,&components, &infile);
	if(imgbuf==NULL) {
		printf("egi_imgbuf_loadjpg(): open_jpgImg() fails!\n");
		return -1;
	}

	/* prepare image buffer */
	egi_imgbuf->height=height;
	egi_imgbuf->width=width;
	EGI_PDEBUG(DBG_BMPJPG,"egi_imgbuf_loadjpg():succeed to open jpg file %s, width=%d, height=%d\n",
								fpath,egi_imgbuf->width,egi_imgbuf->height);
	/* alloc imgbuf */
	egi_imgbuf->imgbuf=malloc(width*height*btypp);
	if(egi_imgbuf->imgbuf==NULL)
	{
		printf("egi_imgbuf_loadjpg(): fail to malloc imgbuf.\n");
		return -2;
	}
	memset(egi_imgbuf->imgbuf,0,width*height*btypp);

	/* TODO: WARNING: need to be improve here: converting 8bit to 24bit color*/
	if(components==1) /* 8bit color */
	{
		printf(" egi_imgbuf_loadjpg(): WARNING!!!! components is 1. \n");
		height=height/3; /* force to be 24bit pic, however shorten the height */
	}

	/* flip picture to be same data sequence of BMP file */
	dat=imgbuf;

	for(i=height-1;i>=0;i--) /* row */
	{
		for(j=0;j<width;j++)
		{
			location= (height-i-1)*width*btypp + j*btypp;

			color=COLOR_RGB_TO16BITS(*dat,*(dat+1),*(dat+2));
			*(uint16_t *)(egi_imgbuf->imgbuf+location/btypp )=color;
			dat +=3;
		}
	}

	close_jpgImg(imgbuf);
	fclose(infile);
	return 0;
}

/*------------------------------------------------------------------------
	Release imgbuf of an EGI_IMGBUf struct.
-------------------------------------------------------------------------*/
void egi_imgbuf_release(EGI_IMGBUF *egi_imgbuf)
{
	if(egi_imgbuf != NULL && egi_imgbuf->imgbuf != NULL)
		free(egi_imgbuf->imgbuf);
}



#if 0
/*---------------------- FULL SCREEN : OBSELET!!! ------------------------------------
For 16bits color only!!!!

Write image data of an EGI_IMGBUF to FB to display it.

egi_imgbuf:	an EGI_IMGBUF struct which hold bits_color image data of a picture.
(xp,yp):	coodinate of the origin(left top) point of LCD relative to
		the coordinate system of the picture(also origin at left top).
Return:
		0 	ok
		<0	fails
---------------------------------------------------------------------------------------*/
int egi_imgbuf_display(const EGI_IMGBUF *egi_imgbuf, FBDEV *fb_dev, int xp, int yp)
{
	/* check data */
	if(egi_imgbuf == NULL)
	{
		printf("egi_imgbuf_display(): egi_imgbuf is NULL. fail to display.\n");
		return -1;
	}

	int i,j;
	int xres=fb_dev->vinfo.xres;
	int yres=fb_dev->vinfo.yres;
	int imgw=egi_imgbuf->width;	/* image Width and Height */
	int imgh=egi_imgbuf->height;
	//printf("egi_imgbuf_display(): imgW=%d, imgH=%d. \n",imgw, imgh);
	unsigned char *fbp =fb_dev->map_fb;
	uint16_t *imgbuf = egi_imgbuf->imgbuf;
	long int locfb=0; /* location of FB mmap, in byte */
	long int locimg=0; /* location of image buf, in byte */
	int btypp=2; /* bytes per pixel */

	for(i=0;i<yres;i++) /* FB row */
	{
		for(j=0;j<xres;j++) /* FB column */
		{
			/* FB location */
			locfb = i*xres*btypp+j*btypp;
			/* NOT necessary ???  check if no space left for a 16bit_pixel in FB mem */
                	if( locfb<0 || locfb>(fb_dev->screensize-btypp) )
                	{
                                 printf("show_bmp(): WARNING: point location out of fb mem.!\n");
                                 return -2;
                	}

			/* check if exceed image boundary */
			if( ( xp+j > imgw-1 || xp+j <0 ) || ( yp+i > imgh-1 || yp+i <0 ) )
			{
				*(uint16_t *)(fbp+locfb)=0; /* black for outside */
			}
			else
			{
				locimg= (i+yp)*imgw*btypp+(j+xp)*btypp; /* image location */
				/*  FB from EGI_IMGBUF */
				*(uint16_t *)(fbp+locfb)=*(uint16_t *)(imgbuf+locimg/btypp);
			}
		}
	}

	return 0;
}
#endif


/*-------------------------     SCREEN WINDOW   -----------------------------------------
For 16bits color only!!!!

1. Write image data of an EGI_IMGBUF to a window of FB to display it.
2. Set outside color as black.
3. window(xw,yw) defines a looking window to the original picture, (xp,yp) is the left_top
   start point of the window. If the looking window covers area ouside of the picture,then
   those area will be filled with BLACK.

egi_imgbuf:	an EGI_IMGBUF struct which hold bits_color image data of a picture.
(xp,yp):	coodinate of the displaying window origin(left top) point, relative to
		the coordinate system of the picture(also origin at left top).
(xw,yw):	displaying window origin, relate to the LCD coord system.
winw,winh:	width and height(row/column for fb) of the displaying window.
---------------------------------------------------------------------------------------*/
int egi_imgbuf_windisplay(const EGI_IMGBUF *egi_imgbuf, FBDEV *fb_dev, int xp, int yp,
				int xw, int yw, int winw, int winh)
{
	/* check data */
	if(egi_imgbuf == NULL)
	{
		printf("egi_imgbuf_display(): egi_imgbuf is NULL. fail to display.\n");
		return -1;
	}
	int imgw=egi_imgbuf->width;	/* image Width and Height */
	int imgh=egi_imgbuf->height;
	if( imgw<0 || imgh<0 )
	{
		printf("egi_imgbuf_display(): egi_imgbuf->width or height is negative. fail to display.\n");
		return -1;
	}

	int i,j;
	int xres=fb_dev->vinfo.xres;
	//int yres=fb_dev->vinfo.yres;

	//printf("egi_imgbuf_display(): imgW=%d, imgH=%d. \n",imgw, imgh);
	unsigned char *fbp =fb_dev->map_fb;
	uint16_t *imgbuf = egi_imgbuf->imgbuf;
	long int locfb=0; /* location of FB mmap, in byte */
	long int locimg=0; /* location of image buf, in byte */
	int btypp=2; /* bytes per pixel */

	//for(i=0;i<yres;i++) /* FB row */
	for(i=0;i<winh;i++) /* row of the displaying window */
	{
		//for(j=0;j<xres;j++) /* FB column */
		for(j=0;j<winw;j++)
		{
			/* FB data location */
//replaced by draw_dot()	locfb = (i+yw)*xres*btypp+(j+xw)*btypp;

			/* check if exceed image boundary */
			if( ( xp+j > imgw-1 || xp+j <0 ) || ( yp+i > imgh-1 || yp+i <0 ) )
			{
//replaced by draw_dot()	*(uint16_t *)(fbp+locfb)=0; /* black for outside */
				fbset_color(0); /* black for outside */
				draw_dot(fb_dev,j+xw,i+yw); /* call draw_dot */
			}
			else
			{
				/* image data location */
				locimg= (i+yp)*imgw*btypp+(j+xp)*btypp;
				/*  FB from EGI_IMGBUF */
//replaced by draw_dor()	*(uint16_t *)(fbp+locfb)=*(uint16_t *)(imgbuf+locimg/btypp);

				/*  ---- draw_dot() here ---- */
				fbset_color(*(uint16_t *)(imgbuf+locimg/btypp));
				draw_dot(fb_dev,j+xw,i+yw); /* call draw_dot */
			}
		}
	}

	return 0;
}


/*--------------------------------------------------------------------------------
Roam a picture in a displaying window

path:		jpg file path
step:		roaming step length, in pixel
ntrip:		number of trips for roaming.
(xw,yw):	displaying window origin, relate to the LCD coord system.
winw,winh:		width and height of the displaying window.
---------------------------------------------------------------------------------*/
int egi_roampic_inwin(char *path, FBDEV *fb_dev, int step, int ntrip,
						int xw, int yw, int winw, int winh)
{
	int i,k;
        int stepnum;

        EGI_POINT pa,pb; /* 2 points define a picture image box */
        EGI_POINT pn; /* origin point of displaying window */
        EGI_IMGBUF  imgbuf={0}; /* u16 color image buffer */

	/* load jpg image to the image buffer */
        egi_imgbuf_loadjpg(path, &imgbuf);

        /* define left_top and right_bottom point of the picture */
        pa.x=0;
        pa.y=0;
        pb.x=imgbuf.width-1;
        pb.y=imgbuf.height-1;

        /* define a box, within which the displaying origin(xp,yp) related to the picture is limited */
        EGI_BOX box={ pa, {pb.x-winw,pb.y-winh}};

	/* set  start point */
        egi_randp_inbox(&pb, &box);

        for(k=0;k<ntrip;k++)
        {
                /* METHOD 1:  pick a random point in box for pn, as end point of this trip */
                //egi_randp_inbox(&pn, &box);

                /* METHOD 2: pick a random point on box sides for pn, as end point of this trip */
                egi_randp_boxsides(&pn, &box);
                printf("random point: pn.x=%d, pn.y=%d\n",pn.x,pn.y);

                /* shift pa pb */
                pa=pb; /* now pb is starting point */
                pb=pn;

                /* count steps from pa to pb */
                stepnum=egi_numstep_btw2p(step,&pa,&pb);
                /* walk through those steps, displaying each step */
                for(i=0;i<stepnum;i++)
                {
                        /* get interpolate point */
                        egi_getpoit_interpol2p(&pn, step*i, &pa, &pb);
			/* display in the window */
                        egi_imgbuf_windisplay( &imgbuf, &gv_fb_dev, pn.x, pn.y, xw, yw, winw, winh ); /* use window */
                        tm_delayms(55);
                }
        }

        egi_imgbuf_release( &imgbuf );

	return 0;
}


/* --------------  OBSELETE!!! see egi_alloc_search_files() in egi_utils.c ----------------
find out all jpg files in a specified directory

path:	 	path for file searching
count:	 	total number of jpg files found
fpaths:  	file path list
maxfnum:	max items of fpaths
maxflen:	max file name length

return value:
         0 --- OK
        <0 --- fails
------------------------------------------------------------------------------------------*/
int egi_find_jpgfiles(const char* path, int *count, char **fpaths, int maxfnum, int maxflen)
{
        DIR *dir;
        struct dirent *file;
        int fn_len;
	int num=0;

        /* open dir */
        if(!(dir=opendir(path)))
        {
                printf("egi_find_jpgfs(): error open dir: %s !\n",path);
                return -1;
        }

	/* get jpg files */
        while((file=readdir(dir))!=NULL)
        {
                /* find out all jpg files */
                fn_len=strlen(file->d_name);
		if(fn_len>maxflen-10)/* file name length limit, 10 as for extend file name */
			continue;
                if(strncmp(file->d_name+fn_len-4,".jpg",4)!=0 )
                         continue;
		sprintf(fpaths[num],"%s/%s",path,file->d_name);
                //strncpy(fpaths[num++],file->d_name,fn_len);
		num++;
		if(num==maxfnum)/* break if fpaths is full */
			break;
        }

	*count=num; /* return count */

         closedir(dir);
         return 0;
}


/*---------------------------------------------------
Save FB data to a BMP file.

TODO: RowSize=4*[ BPP*Width/32], otherwise padded with 0.

fpath:	Input path to the file.

Return:
	0	OK
	<0	Fail
---------------------------------------------------*/
int egi_save_FBbmp(FBDEV *fb_dev, const char *fpath)
{
	FILE *fil;
	int rc;
	int i,j;
	int xres=fb_dev->vinfo.xres; /* line pixel number */
	int yres=fb_dev->vinfo.yres;
	EGI_16BIT_COLOR	color16b;
	PIXEL bgr;
	BITMAPFILEHEADER file_header; /* 14 bytes */
	BITMAPINFOHEADER info_header; /* 40 bytes */

   printf("file header size:%d,	info header size:%d\n",sizeof(BITMAPFILEHEADER),sizeof(BITMAPINFOHEADER));

	/* fill in file header */
	memset(&file_header,0,sizeof(BITMAPFILEHEADER));
	file_header.cfType[0]='B';
	file_header.cfType[1]='M';
	file_header.cfSize=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)
					+fb_dev->screensize/2*3;  /* 16bits -> 24bits */
	file_header.cfReserved=0;
	file_header.cfoffBits=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
/*
        char cfType[2];//文件类型，"BM"(0x4D42)
        long cfSize;//文件大小（字节）
        long cfReserved;//保留，值为0
        long cfoffBits;//数据区相对于文件头的偏移量（字节）
*/

	/* Fill in info header */
	memset(&info_header,0,sizeof(BITMAPINFOHEADER));
	info_header.ciSize[0]=40;
	info_header.ciWidth=240;
	info_header.ciHeight=320;
	info_header.ciPlanes[0]=1;
	info_header.ciBitCount=24;
	*(long *)info_header.ciSizeImage=fb_dev->screensize/2*3; /* to be multiple of 4 */
/*
        char ciSize[4];//BITMAPFILEHEADER所占的字节数
        long ciWidth;//宽度
        long ciHeight;//高度
        char ciPlanes[2];//目标设备的位平面数，值为1
        int ciBitCount;//每个像素的位数
        char ciCompress[4];//压缩说明
        char ciSizeImage[4];//用字节表示的图像大小，该数据必须是4的倍数
        char ciXPelsPerMeter[4];//目标设备的水平像素数/米
        char ciYPelsPerMeter[4];//目标设备的垂直像素数/米
        char ciClrUsed[4]; //位图使用调色板的颜色数
        char ciClrImportant[4]; //
*/

	/* open file for write */
	fil=fopen(fpath,"w");
	if(fil==NULL) {
		printf("%s: Fail to open %s for write.\n",__func__,fpath);
		return -1;
	}

	/* write file header */
        rc = fwrite((char *)&file_header, 1, sizeof(BITMAPFILEHEADER), fil);
        if (rc != sizeof(BITMAPFILEHEADER)) {
		printf("%s: Fail to write file header to %s.\n",__func__,fpath);
		fclose(fil);
		return -2;
         }
	/* write info header */
        rc = fwrite((char *)&info_header, 1, sizeof(BITMAPINFOHEADER), fil);
        if (rc != sizeof(BITMAPINFOHEADER)) {
		printf("%s: Fail to write info header to %s.\n",__func__,fpath);
		fclose(fil);
		return -3;
         }

	/* RGB color saved in order of BGR in BMP file
	 * From bottom to up
	 */
	for(i=0;i<yres;i++) /* iterate line number */
	{
		for(j=0;j<xres;j++) /* iterate pixels in a line, xres to be multiple of 4 */
		{
			color16b=*(uint16_t *)(fb_dev->map_fb+fb_dev->screensize-(i+1)*xres*2 + j*2); /* bpp=2 */
			bgr.red=(color16b>>11)<<3;
			bgr.green=(color16b&0b11111100000)>>3;
			bgr.blue=(color16b << 11)>>8;

		        rc = fwrite((char *)&bgr, 1, sizeof(PIXEL), fil);
       			if (rc != sizeof(PIXEL)) {
				printf("%s: Fail to write BGR data to %s, i=%d. \n",__func__,fpath,i);
				fclose(fil);
				return -4;
			}
         	}
	}

	fclose(fil);
	return 0;
}
