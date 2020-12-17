/* 3-D modeling/migration with extended SSF */

/*
  Copyright (C) 2009 Colorado School of Mines

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <rsf.h>

#ifdef _OPENMP
#include <omp.h>
#include "omputil.h"
#endif

#include "wex.h"
#include "wexeic.h"
#include "wextap.h"
#include "wexssr.h"
#include "wexslo.h"
#include "wexutl.h"
/*^*/

int main (int argc, char *argv[])
{
    bool verb;            /* verbosity */
    bool causal;          /* causal/anti-causal flag */
    float eps;            /* dip filter constant */
    int  adj;             /* forward or adjoint */
    int save;             /* save wavefield or not */
    int eic;              /* output cic or eic cube */
    int   nrmax;          /* number of reference velocities */
    float dtmax;          /* time error */
    int   pmx,pmy;        /* padding in the k domain */
    int   tmx,tmy;        /* boundary taper size */
    int wexsign;

    int iw;

    int nhx, nhy, nhz, nht, nc;
    int nhx2,nhy2,nhz2,nht2;
    float dht = 0.0, oht = 0.0;

    sf_axis amx,amy,az;
    sf_axis alx,aly;
    sf_axis aw,ae,aa,ac;
    sf_axis ahx,ahy,ahz,aht;

    /* I/O files */
    sf_file Fs=NULL;    /*  slowness file S(nlx,nly,nz   ) */
    sf_file Fc=NULL;    /*  CIP coordinates                */
    sf_file Fd=NULL;    /*  if adj=0, then modeling, Fd is 
			    output surface data file D(nmx,nmy,nw); 
			    otherwise Fd is input surface data 
                            file D(nmx,nmy,nw)             */
    sf_file Fm=NULL;    /*  if adj=0, Fm is input reflectivity 
                            r(nmx,nmy,nmz); otherwise Fm is output 
			    image r(nmx,nmy,nmz) or CIP 
			    gathers                        */
    sf_file Fws=NULL;   /*  source wavefield               */
    sf_file Fwr=NULL;   /*  receiver wavefield             */
    sf_file Fis=NULL;   /*  injected source wavefield      */
    sf_file Ftw=NULL;   /*  temp wavefield                 */

    /* temp arrays */
    sf_complex ***wtmp;

    int ompnth=1;

    wexcub3d cub; /* wavefield hypercube */
    wexcip3d cip; /* CIP gathers */
    wextap3d tap; /* tapering */
    wexssr3d ssr; /* SSR operator */
    wexslo3d slo; /* slowness */

    wexop3d weop;

    float dsmax;

    /*------------------------------------------------------------*/
    sf_init(argc,argv);

    /* OMP parameters */
#ifdef _OPENMP
    ompnth=omp_init();
#endif

    if (!sf_getint(    "adj",&adj   ))  sf_error("Specify the adjoint!"); /* y=ADJ migration; n=FWD modeling */
    if (!sf_getint(   "save",&save  ))  save = 0;     /* save wfld flag */
    if (!sf_getint(   "feic",&eic   ))   eic = 0;     /* extended I.C. flag */
    if (!sf_getbool(  "verb",&verb  ))  verb = false; /* verbosity flag */
    if (!sf_getfloat(  "eps",&eps   ))   eps =  0.01; /* stability parameter */
    if (!sf_getint(  "nrmax",&nrmax )) nrmax =     1; /* maximum references */
    if (!sf_getfloat("dtmax",&dtmax )) dtmax = 0.004; /* max time error */

    if (!sf_getint(    "pmx",&pmx   ))   pmx =     0; /* padding on x */
    if (!sf_getint(    "pmy",&pmy   ))   pmy =     0; /* padding on y*/

    if (!sf_getint(    "tmx",&tmx   ))   tmx =     0; /* taper on x*/
    if (!sf_getint(    "tmy",&tmy   ))   tmy =     0; /* taper on y */

    if(adj) causal= false; else causal= true;

    ae  = sf_maxa(1,0,1);
    nhx=nhy=nhz=nht=nc=nhx2=nhy2=nhz2=nht2=0;
    oht = 0.0;

    /*------------------------------------------------------------*/
    /* slowness */

    Fs = sf_input("slo");
    alx = sf_iaxa(Fs,1); sf_setlabel(alx,"lx");
    aly = sf_iaxa(Fs,2); sf_setlabel(aly,"ly");
    az =  sf_iaxa(Fs,3); sf_setlabel(az, "z");

    /*------------------------------------------------------------*/
    /* wavefield */
    Fws = sf_input ( "swfl");
    amx = sf_iaxa(Fws,1); sf_setlabel(amx,"mx");
    amy = sf_iaxa(Fws,2); sf_setlabel(amy,"my");
    aw  = sf_iaxa(Fws,4); sf_setlabel(aw ,"w" );

    /*------------------------------------------------------------*/
    cub = wex_cube(verb,
                   amx,amy,az,
                   alx,aly,
                   aw,
                   ae,
                   eps,
                   ompnth);
    dsmax = dtmax/cub->az.d;

    /*------------------------------------------------------------*/
    /* init structures */
    tap = wextap_init(cub->amx.n,
                      cub->amy.n,
                      1,
                      SF_MIN(tmx,cub->amx.n-1), /* tmx */
                      SF_MIN(tmy,cub->amy.n-1), /* tmy */
                      0,
                      true,true,false);
    slo = wexslo_init(cub,Fs,nrmax,dsmax);
    ssr = wexssr_init(cub,slo,pmx,pmy,tmx,tmy,dsmax);
    weop = wex_init(cub);

    wexsign=causal?+1:-1;

    /*------------------------------------------------------------*/
    Ftw = sf_tmpfile(NULL); sf_settype(Ftw,SF_COMPLEX);
    Fis = sf_tmpfile(NULL); sf_settype(Fis,SF_COMPLEX);

    /*------------------------------------------------------------*/
    if(!adj){ /* For modeling, output surface data */
        sf_warning("modeling ....");

        Fm = sf_input("in");
	if(eic){
	    ahx = sf_iaxa(Fm,1); sf_setlabel(ahx,"hx");
	    ahy = sf_iaxa(Fm,2); sf_setlabel(ahy,"hy");
	    ahz = sf_iaxa(Fm,3); sf_setlabel(ahz,"hz");
	    aht = sf_iaxa(Fm,4); sf_setlabel(aht,"ht");

	    nhx2 = sf_n(ahx); nhx = (nhx2-1)/2;
	    nhy2 = sf_n(ahy); nhy = (nhy2-1)/2;
	    nhz2 = sf_n(ahz); nhz = (nhz2-1)/2;
	    nht2 = sf_n(aht); nht = (nht2-1)/2;
  
            dht = sf_d(aht);  oht = sf_o(aht);

	    Fc = sf_input ("cc" );
	    ac = sf_iaxa(Fc,2); sf_setlabel(ac,"cc"); sf_setunit(ac,"");
	    nc = sf_n(ac);
        }
        cip = wexcip_init(cub,nhx,nhy,nhz,nht,nhx2,nhy2,nhz2,nht2,nc,dht,oht,Fc,eic);

	Fd = sf_output("out"); sf_settype(Fd,SF_COMPLEX);
        sf_oaxa(Fd,amx,1);
        sf_oaxa(Fd,amy,2);
        sf_oaxa(Fd,aw, 3);
        if(eic){
            sf_oaxa(Fd,ae, 4);
            sf_oaxa(Fd,ae, 5);
        }

	wexcip_adj(cub,cip,Fws,Fis,Fm,eic,1,0);
        sf_filefresh(Fis);

        // wavefield extrapolation 
        wex(weop,cub,ssr,tap,slo,wexsign,Fis,Ftw,adj);

        // allocation for temp arrays 
        wtmp = sf_complexalloc3(cub->amx.n,cub->amy.n,cub->aw.n);

        for(iw=0; iw<cub->aw.n; iw++){
            sf_seek(Ftw,sizeof(sf_complex)*cub->amx.n*cub->amy.n*cub->az.n*iw,SEEK_SET);
            sf_complexread(wtmp[iw][0],cub->amx.n*cub->amy.n,Ftw);
        }
        sf_complexwrite(wtmp[0][0],cub->amx.n*cub->amy.n*cub->aw.n,Fd);

        /* deallocate temp arrays */
        free(**wtmp );free(*wtmp );free( wtmp );
    }
    else{ /* For migration, output image or CIPs */
        sf_warning("migration ....");

        Fd = sf_input(  "in");
        if (SF_COMPLEX !=sf_gettype(Fd)) sf_error("Need complex data");
        Fm = sf_output("out"); sf_settype(Fm,SF_COMPLEX);

	if(eic){
	    /* CIP coordinates */
	    Fc = sf_input ("cc" ); 
	    ac = sf_iaxa(Fc,2); sf_setlabel(ac,"cc"); sf_setunit(ac,"");
	    nc = sf_n(ac);

	    if(! sf_getint("nhx",&nhx)) nhx=0; /* number of lags on the x axis */
	    if(! sf_getint("nhy",&nhy)) nhy=0; /* number of lags on the y axis */
	    if(! sf_getint("nhz",&nhz)) nhz=0; /* number of lags on the z axis */
	    if(! sf_getint("nht",&nht)) nht=0; /* number of lags on the t axis */
	    if(! sf_getfloat("dht",&dht)) sf_error("need dht");
	    oht = -nht*dht;

	    nhx2=2*nhx+1;
	    nhy2=2*nhy+1;
	    nhz2=2*nhz+1;
	    nht2=2*nht+1;
	    
	    aa=sf_maxa(nhx2,-nhx*cub->amx.d,cub->amx.d);
	    sf_setlabel(aa,"hx"); sf_setunit(aa,"");
	    if(verb) sf_raxa(aa);
	    sf_oaxa(Fm,aa,1);
	    
	    aa=sf_maxa(nhy2,-nhy*cub->amy.d,cub->amy.d);
	    sf_setlabel(aa,"hy"); sf_setunit(aa,"");
	    if(verb) sf_raxa(aa);
	    sf_oaxa(Fm,aa,2);
	    
	    aa=sf_maxa(nhz2,-nhz*cub->az.d,cub->az.d);
	    sf_setlabel(aa,"hz"); sf_setunit(aa,"");
	    if(verb) sf_raxa(aa);
	    sf_oaxa(Fm,aa,3);
	
	    aa=sf_maxa(nht2,-nht*dht,dht);
	    sf_setlabel(aa,"ht"); sf_setunit(aa,"s");
	    if(verb) sf_raxa(aa);
	    sf_oaxa(Fm,aa,4);
	    
	    sf_oaxa(Fm,ac,5);
	}
	else{
	    sf_oaxa(Fm,amx,1);
	    sf_oaxa(Fm,amy,2);
	    sf_oaxa(Fm,az, 3);
	}

        /* receiver wavefield extrapolation */
        wex(weop,cub,ssr,tap,slo,wexsign,Fd,Ftw,adj);

        /* initialize CIP gathers */
        cip = wexcip_init(cub,nhx,nhy,nhz,nht,nhx2,nhy2,nhz2,nht2,nc,dht,oht,Fc,eic);

        /* construct CIP gathers */
        wexcip_for(cub,cip,Fws,Ftw,Fm,eic,0,0);

        if(save){
            Fwr = sf_output("rwfl"); sf_settype(Fwr,SF_COMPLEX);
            sf_oaxa(Fwr,amx,1);
            sf_oaxa(Fwr,amy,2);
            sf_oaxa(Fwr,az, 3);
            sf_oaxa(Fwr,aw, 4);

            sf_filefresh(Ftw);
            sf_filecopy(Fwr,Ftw,SF_COMPLEX);
        }
    }

    /*------------------------------------------------------------*/
    /* close structures */
    wex_close(weop);
    wexslo_close(slo);
    wexssr_close(cub,ssr);
    wextap2D_close(tap);
    if(eic) wexcip_close(cip,eic);
    /*------------------------------------------------------------*/

    /*------------------------------------------------------------*/
    /* close files */
    if (Fs!=NULL) sf_fileclose(Fs);
    if (Fc!=NULL) sf_fileclose(Fc);
    if (Fd!=NULL) sf_fileclose(Fd);
    if (Fm!=NULL) sf_fileclose(Fm);
    if (Fws!=NULL) sf_fileclose(Fws);
    if (Fwr!=NULL) sf_fileclose(Fwr);
    if (Fis!=NULL) sf_tmpfileclose(Fis);
    if (Ftw!=NULL) sf_tmpfileclose(Ftw);
    /*------------------------------------------------------------*/

    exit (0);
}
         
/*












        filename = sf_getstring("rwfl");
        if(NULL==filename) filename="rwfl";
        sf_warning("%s",filename);







*/
