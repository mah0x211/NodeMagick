#include <node.h>
#include <node_events.h>

#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/time.h>
#include <limits.h>

#include <cstring>
#include <typeinfo>
#include <pthread.h>
#include "wand/MagickWand.h"

using namespace v8;
using namespace node;

#define ObjectUnwrap(tmpl,obj)  ObjectWrap::Unwrap<tmpl>(obj)

#define IsDefined(v) ( !v->IsNull() && !v->IsUndefined() )

#define WandStrError(wand,str)({ \
  ExceptionType severity; \
  char *desc = MagickGetException( wand, &severity ); \
  printf("error: %s\n",desc); \
  str = String::New(desc); \
  MagickRelinquishMemory(desc); \
})

typedef enum {
    ALIGN_NONE,
    ALIGN_LEFT = 1,
    ALIGN_CENTER,
    ALIGN_RIGHT,

    ALIGN_TOP = 1,
    ALIGN_MIDDLE,
    ALIGN_BOTTOM
} ImageAlign_e;

typedef struct {
    unsigned long w;
    unsigned long h;
    double aspect;
} ImageSize;


typedef enum ASYNC_TASK_BIT {
    ASYNC_TASK_LOAD = 1 << 0,
    ASYNC_TASK_SAVE = 1 << 1
};
typedef struct {
    void *ctx;
    int task;
    const char *errstr;
    void *udata;
    // callback js function when async is true
    Persistent<Function> callback;
    Handle<Value> retval;
    eio_req *req;
} Baton_t;

static pthread_mutex_t mutex;

// MARK: @interface
class NodeMagick : public ObjectWrap
{
    // MARK: @public
    public:
        NodeMagick();
        ~NodeMagick();
        static void Initialize( Handle<Object> target );
    // MARK: @private
    private:
        MagickWand *wand;
        int attached;
        const char *format;
        const char *format_to;
        const char *src;
        unsigned int quality;
        double scale;
        int cropped;
        int resized;
        long x;
        long y;
        ImageSize size;
        ImageSize crop;
        ImageSize resize;
        
        // new
        static Handle<Value> New( const Arguments& argv );
        Handle<Value> loadImage( const char *path );
        Handle<Value> saveImage( const char *path );

        // setter/getter
        static Handle<Value> getFormat( Local<String> prop, const AccessorInfo &info );
        static void setFormat( Local<String> prop, Local<Value> val, const AccessorInfo &info );
        static Handle<Value> getRawWidth( Local<String> prop, const AccessorInfo &info );
        static Handle<Value> getRawHeight( Local<String> prop, const AccessorInfo &info );
        static Handle<Value> getWidth( Local<String> prop, const AccessorInfo &info );
        static Handle<Value> getHeight( Local<String> prop, const AccessorInfo &info );
        static Handle<Value> getQuality( Local<String> prop, const AccessorInfo &info );
        static void setQuality( Local<String> prop, Local<Value> val, const AccessorInfo &info );
        
        static Handle<Value> fnCrop( const Arguments &argv );
        static Handle<Value> fnScale( const Arguments& argv );
        static Handle<Value> fnResize( const Arguments& argv );
        static Handle<Value> fnResizeByWidth( const Arguments& argv );
        static Handle<Value> fnResizeByHeight( const Arguments& argv );
        static Handle<Value> fnLoad( const Arguments& argv );
        static Handle<Value> fnSave( const Arguments& argv );
        
        // thread task
        static int beginEIO( eio_req *req );
        static int endEIO( eio_req *req );
};

// MARK: @implements
NodeMagick::NodeMagick()
{
    wand = NewMagickWand();
    attached = 0;
    format = NULL;
    format_to = NULL;
    src = NULL;
    quality = 100;
    scale = 100.0;
    cropped = resized = 0;
    x = y = 0;
    size.w = crop.w = resize.w = 0;
    size.h = crop.h = resize.h = 0;
    size.aspect = crop.aspect = 1;
}

NodeMagick::~NodeMagick()
{
    if( src ){
        free( (void*)src );
    }
    if( format_to ){
        free( (void*)format_to );
    }
    if( wand ){
        DestroyMagickWand(wand);
    }
}


int NodeMagick::beginEIO( eio_req *req )
{
    Baton_t *baton = static_cast<Baton_t*>( req->data );
    NodeMagick *ctx = (NodeMagick*)baton->ctx;
    
    // failed to lock mutex
    if( pthread_mutex_lock( &mutex ) ){
        baton->errstr = strerror(errno);
    }
    else
    {
        if( baton->task & ASYNC_TASK_LOAD ){
            baton->retval = ctx->loadImage( (const char*)baton->udata );
        }
        else if( baton->task & ASYNC_TASK_SAVE ){
            baton->retval = ctx->saveImage( (const char*)baton->udata );
        }
        
        // failed to unlock mutex
        if( pthread_mutex_unlock( &mutex ) ){
            baton->errstr = strerror(errno);
        }
    }
    
    return 0;
}

int NodeMagick::endEIO( eio_req *req )
{
    HandleScope scope;
    Baton_t *baton = static_cast<Baton_t*>(req->data);
    NodeMagick *ctx = (NodeMagick*)baton->ctx;
    int task = baton->task;
    Local<Function> cb = Local<Function>::New( baton->callback );
    Handle<Primitive> t = Undefined();
    Local<Value> errstr = reinterpret_cast<Local<Value>&>(t);
    Local<Value> argv[] = {
        reinterpret_cast<Local<Value>&>(errstr)
    };

    ev_unref(EV_DEFAULT_UC);
    ctx->Unref();
    
    if( baton->errstr ){
        errstr = Exception::Error( String::New( baton->errstr ) );
    }
    else if( IsDefined( baton->retval ) ){
        errstr = Exception::Error( baton->retval->ToString() );
    }
    
    // cleanup
    baton->callback.Dispose();
    if( baton->udata ){
        free((void*)baton->udata);
    }
    delete baton;
    
    TryCatch try_catch;
    // call js function by callback function context
    // !!!: which is better callback or Context::GetCurrent()->Global() context
    cb->Call( ctx->handle_, 1, argv );
    if( try_catch.HasCaught() ){
        FatalException(try_catch);
    }
    
    eio_cancel(req);
    
    return 0;
}

Handle<Value> NodeMagick::New( const Arguments& argv )
{
    HandleScope scope;
    NodeMagick *ctx = new NodeMagick();
    Handle<Value> retval = Undefined();
    
    if( !ctx->wand ){
        delete ctx;
        retval = ThrowException( Exception::Error( String::New(strerror(ENOMEM)) ) );
    }
    else {
        ctx->Wrap( argv.This() );
        retval = argv.This();
    }
    
    return scope.Close( retval );
}


Handle<Value> NodeMagick::loadImage( const char *path )
{
    Handle<Value> retval = Undefined();
    MagickBooleanType status;
    
    if( attached ){
        wand = DestroyMagickWand( wand );
        wand = NewMagickWand();
        attached = 0;
        free( (void*)src );
    }
    if( ( status = MagickReadImage( wand, path ) ) == MagickFalse ){
        WandStrError(wand,retval);
    }
    else {
        attached = 1;
        src = strdup(path);
        format = MagickGetImageFormat( wand );
        size.w = crop.w = resize.w = MagickGetImageWidth( wand );
        size.h = crop.h = resize.h = MagickGetImageHeight( wand );
        size.aspect = crop.aspect = (double)size.w/(double)size.h;
    }
    
    return retval;
}

Handle<Value> NodeMagick::fnLoad( const Arguments& argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Undefined();
    const int argc = argv.Length();
    bool callback = false;

    if( argc < 1 || 
        !argv[0]->IsString() || !argv[0]->ToString()->Length() ||
        ( argc > 1 && !( callback = argv[1]->IsFunction() ) ) ){
        retval = ThrowException( Exception::TypeError( String::New( "load( path_to_image:String, [callback:Function] )" ) ) );
    }
    else if( callback )
    {
        Baton_t *baton = new Baton_t();
        
        baton->task = ASYNC_TASK_LOAD;
        baton->ctx = (void*)ctx;
        baton->retval = Undefined();
        baton->errstr = NULL;
        baton->udata = strdup( *String::Utf8Value( argv[0] ) );
        // detouch from GC
        baton->callback = Persistent<Function>::New( Local<Function>::Cast( argv[1] ) );
        ctx->Ref();
        baton->req = eio_custom( beginEIO, EIO_PRI_DEFAULT, endEIO, baton );
        ev_ref(EV_DEFAULT_UC);
    }
    else
    {
        retval = ctx->loadImage( *String::Utf8Value( argv[0] ) );
        // failed
        if( IsDefined( retval ) ){
            retval = ThrowException( Exception::Error( retval->ToString() ) );
        }
    }
    
    return scope.Close( retval );
}

Handle<Value> NodeMagick::saveImage( const char *path )
{
    Handle<Value> retval = Undefined();
    MagickBooleanType status = MagickTrue;
    
    if( attached )
    {
        // crop
        if( cropped ){
            status = MagickCropImage( wand, crop.w, crop.h, x, y );
        }
        // resize
        if( status == MagickTrue && resized ){
            status = MagickSampleImage( wand, resize.w, resize.h );
        }
        // quality 0-100
        if( status == MagickTrue ){
            status = MagickSetImageCompressionQuality( wand, quality );
        }
        // format
        if( status == MagickTrue && format_to ){
            status = MagickSetFormat( wand, format_to );
        }
        // remove profiles
        if( status == MagickTrue ){
            status = MagickProfileImage( wand, "*", NULL, 1 );
        }
        // write
        if( status == MagickTrue ){
            status = MagickWriteImage( wand, path );
        }
        // failed
        if( status == MagickFalse ){
            WandStrError(wand,retval);
        }
    }
    
    return retval;
}

Handle<Value> NodeMagick::fnSave( const Arguments &argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Undefined();
    const int argc = argv.Length();
    bool callback = false;
    
    if( argc < 1 || 
        !argv[0]->IsString() || !argv[0]->ToString()->Length() ||
        ( argc > 1 && !( callback = argv[1]->IsFunction() ) ) ){
        retval = ThrowException( Exception::TypeError( String::New( "save( path_to_file:String, [callback:Function] )" ) ) );
    }
    else if( callback )
    {
        Baton_t *baton = new Baton_t();
        
        baton->task = ASYNC_TASK_SAVE;
        baton->ctx = (void*)ctx;
        baton->retval = Undefined();
        baton->errstr = NULL;
        baton->udata = strdup( *String::Utf8Value( argv[0] ) );
        // detouch from GC
        baton->callback = Persistent<Function>::New( Local<Function>::Cast( argv[1] ) );
        ctx->Ref();
        baton->req = eio_custom( beginEIO, EIO_PRI_DEFAULT, endEIO, baton );
        ev_ref(EV_DEFAULT_UC);
    }
    else {
        const char *path = strdup( *String::Utf8Value( argv[0] ) );
        retval = ctx->saveImage( path );
        free((void*)path);
        // failed
        if( IsDefined( retval ) ){
            retval = ThrowException( Exception::Error( retval->ToString() ) );
        }
    }
    
    return scope.Close( retval );
}

Handle<Value> NodeMagick::getFormat( Local<String>, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    
    return scope.Close( String::New( ctx->format ) );
}

void NodeMagick::setFormat( Local<String>, Local<Value> val, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    
    if( val->IsString() && val->ToString()->Length() ){
        ctx->format_to = strdup( *String::Utf8Value( val ) );
    }
}

Handle<Value> NodeMagick::getRawWidth( Local<String>, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    return scope.Close( Number::New( ctx->size.w ) );
}
Handle<Value> NodeMagick::getRawHeight( Local<String>, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    return scope.Close( Number::New( ctx->size.h ) );
}

Handle<Value> NodeMagick::getWidth( Local<String>, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    return scope.Close( Number::New( ( ctx->resized ) ? ctx->resize.w : ctx->crop.w ) );
}
Handle<Value> NodeMagick::getHeight( Local<String>, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    return scope.Close( Number::New( ( ctx->resized ) ? ctx->resize.h : ctx->crop.h ) );
}

Handle<Value> NodeMagick::getQuality( Local<String>, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    return scope.Close( Number::New( ctx->quality ) );
}
void NodeMagick::setQuality( Local<String>, Local<Value> val, const AccessorInfo &info )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, info.This() );
    
    if( val->IsNumber() )
    {
        ctx->quality = val->Uint32Value();
        if( ctx->quality > 100 ){
            ctx->quality = 100;
        }
    }
}

Handle<Value> NodeMagick::fnCrop( const Arguments &argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Boolean::New( false );
    const int argc = argv.Length();
    double aspect;
    
    if( argc < 1 || !( aspect = argv[0]->NumberValue() ) ){
        retval = ThrowException( Exception::TypeError( String::New( "crop( aspect:Number > 0, align:Number )" ) ) );
    }
    else
    {
        ctx->cropped = 1;
        if( ctx->size.aspect > aspect )
        {
            ctx->crop.w = ctx->size.h * aspect;
            ctx->crop.h = ctx->size.h;
            if( argc > 1 && argv[1]->IsNumber() )
            {
                switch( argv[1]->Uint32Value() )
                {
                    case ALIGN_LEFT:
                        ctx->x = 0;
                    break;
                    
                    case ALIGN_CENTER:
                        ctx->x = ( ctx->size.w - ctx->crop.w ) / 2;
                    break;
                    
                    case ALIGN_RIGHT:
                        ctx->x = ctx->size.w - ctx->crop.w;
                    break;
                    
                    case ALIGN_NONE:
                    break;
                }
            }
        }
        else if( ctx->size.aspect < aspect )
        {
            ctx->crop.h = ctx->size.w / aspect;
            ctx->crop.w = ctx->size.w;
            if( argc > 1 && argv[1]->IsNumber() )
            {
                switch( argv[1]->Uint32Value() )
                {
                    case ALIGN_TOP:
                        ctx->y = 0;
                    break;
                    
                    case ALIGN_MIDDLE:
                        ctx->y = ( ctx->size.h - ctx->crop.h ) / 2;
                    break;
                    
                    case ALIGN_BOTTOM:
                        ctx->y = ctx->size.h - ctx->crop.h;
                    break;
                    
                    case ALIGN_NONE:
                    break;
                }
            }
        }
        else {
            ctx->cropped = 0;
        }
        
        if( ctx->cropped ){
            ctx->crop.aspect = (double)ctx->crop.w/(double)ctx->crop.h;
            retval = Boolean::New( true );
        }
    }
    return scope.Close( retval );
}

Handle<Value> NodeMagick::fnScale( const Arguments &argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Undefined();
    const int argc = argv.Length();
    double per;
    
    if( argc < 1 || !argv[0]->IsNumber() || ( per = argv[0]->NumberValue() ) <= 0.0 ){
        retval = ThrowException( Exception::TypeError( String::New( "scale( percentages:Number > 0 )" ) ) );
    }
    else
    {
        double w = ctx->size.w;
        double h = ctx->size.h;
        
        // if cropped
        if( ctx->cropped ){
            w = ctx->crop.w;
            h = ctx->crop.h;
        }
        
        ctx->scale = per;
        ctx->resize.w = ( w / 100 ) * per;
        ctx->resize.h = ( h / 100 ) * per;
        ctx->resized = 1;
    }
    
    return scope.Close( retval );
}

Handle<Value> NodeMagick::fnResize( const Arguments &argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Undefined();
    const int argc = argv.Length();
    unsigned int width,height;
    
    if( argc < 2 || 
        !argv[0]->IsNumber() || ( width = argv[0]->Uint32Value() ) < 1 ||
        !argv[1]->IsNumber() || ( height = argv[1]->Uint32Value() ) < 1 ){
        retval = ThrowException( Exception::TypeError( String::New( "resize( width:Number > 0, height:Number > 0 )" ) ) );
    }
    else
    {
        double w = ctx->size.w;
        double h = ctx->size.h;
        
        // if cropped
        if( ctx->cropped ){
            w = ctx->crop.w;
            h = ctx->crop.h;
        }
        
        if( w != width || h != height ){
            ctx->resize.w = width;
            ctx->resize.h = height;
            ctx->resized = 1;
        }
    }
    
    return scope.Close( retval );
}

Handle<Value> NodeMagick::fnResizeByWidth( const Arguments &argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Undefined();
    const int argc = argv.Length();
    unsigned int width;
    
    if( argc < 1 || !argv[0]->IsNumber() || ( width = argv[0]->Uint32Value() ) < 1 ){
        retval = ThrowException( Exception::TypeError( String::New( "resizeByWidth( width:Number > 0 )" ) ) );
    }
    else
    {
        double w = ctx->size.w;
        double h = ctx->size.h;
        double aspect = ctx->size.aspect;
        
        // if cropped
        if( ctx->cropped ){
            w = ctx->crop.w;
            h = ctx->crop.h;
            aspect = ctx->crop.aspect;
        }
        
        if( w != width ){
            ctx->resize.w = width;
            ctx->resize.h = width / aspect;
            ctx->resized = 1;
        }
    }
    
    return scope.Close( retval );
}

Handle<Value> NodeMagick::fnResizeByHeight( const Arguments &argv )
{
    HandleScope scope;
    NodeMagick *ctx = ObjectUnwrap( NodeMagick, argv.This() );
    Handle<Value> retval = Undefined();
    const int argc = argv.Length();
    unsigned int height;
    
    if( argc < 1 || !argv[0]->IsNumber() || ( height = argv[0]->Uint32Value() ) < 1 ){
        retval = ThrowException( Exception::TypeError( String::New( "resizeByHeight( height:Number > 0 )" ) ) );
    }
    else
    {
        double w = ctx->size.w;
        double h = ctx->size.h;
        double aspect = ctx->size.aspect;
        
        // if cropped
        if( ctx->cropped ){
            w = ctx->crop.w;
            h = ctx->crop.h;
            aspect = ctx->crop.aspect;
        }
        
        if( h != height ){
            ctx->resize.w = height * aspect;
            ctx->resize.h = height;
            ctx->resized = 1;
        }
    }
    
    return scope.Close( retval );
}


void NodeMagick::Initialize( Handle<Object> target )
{
    HandleScope scope;
    Local<FunctionTemplate> t = FunctionTemplate::New( New );
    
    pthread_mutex_init( &mutex, NULL );
    MagickWandGenesis();
    
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName( String::NewSymbol("NodeMagick") );
    
    NODE_SET_PROTOTYPE_METHOD( t, "crop", fnCrop );
    NODE_SET_PROTOTYPE_METHOD( t, "scale", fnScale );
    NODE_SET_PROTOTYPE_METHOD( t, "resize", fnResize );
    NODE_SET_PROTOTYPE_METHOD( t, "resizeByWidth", fnResizeByWidth );
    NODE_SET_PROTOTYPE_METHOD( t, "resizeByHeight", fnResizeByHeight );
    NODE_SET_PROTOTYPE_METHOD( t, "load", fnLoad );
    NODE_SET_PROTOTYPE_METHOD( t, "save", fnSave );
    
    Local<ObjectTemplate> proto = t->PrototypeTemplate();
    proto->SetAccessor(String::NewSymbol("format"), getFormat, setFormat );
    proto->SetAccessor(String::NewSymbol("quality"), getQuality, setQuality );
    proto->SetAccessor(String::NewSymbol("rawWidth"), getRawWidth );
    proto->SetAccessor(String::NewSymbol("rawHeight"), getRawHeight );
    proto->SetAccessor(String::NewSymbol("width"), getWidth );
    proto->SetAccessor(String::NewSymbol("height"), getHeight );
    
    target->Set( String::NewSymbol("NodeMagick"), t->GetFunction() );
}


extern "C" 
{
    static void init( Handle<Object> target )
    {
        HandleScope scope;
        NodeMagick::Initialize( target );
    }
    NODE_MODULE( NodeMagick, init );
};
