//////////////////////////////////////////////////////////////////////
//
//  ColorOperators.h - Templated operators for dealing with color
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 4, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

// Scalar Multiplication (*) 
inline friend COLOR_CLASS_TYPE operator*( const COLOR_CLASS_TYPE &v, const Scalar t )  
{
	return COLOR_CLASS_TYPE( v[0]*t, v[1]*t, v[2]*t );
}

// Scalar Multiplication (*)
inline friend COLOR_CLASS_TYPE operator*( const Scalar t, const COLOR_CLASS_TYPE &v )  
{ 
	return COLOR_CLASS_TYPE( v[0]*t, v[1]*t, v[2]*t );
}

// Addition (+)
inline friend COLOR_CLASS_TYPE operator+( const COLOR_CLASS_TYPE &a, const COLOR_CLASS_TYPE &b )  
{
	return COLOR_CLASS_TYPE( a[0] + b[0], a[1] + b[1], a[2] + b[2] );
}

// Subtraction (-)
inline friend COLOR_CLASS_TYPE operator-( const COLOR_CLASS_TYPE &a, const COLOR_CLASS_TYPE &b )  
{
	return COLOR_CLASS_TYPE( a[0] - b[0], a[1] - b[1], a[2] - b[2] );
}

// Modulate
inline friend COLOR_CLASS_TYPE operator*( const COLOR_CLASS_TYPE &a, const COLOR_CLASS_TYPE &b )  
{
	return COLOR_CLASS_TYPE( a[0] * b[0], a[1] * b[1], a[2] * b[2] );
}

// Divide
inline friend COLOR_CLASS_TYPE operator/( const COLOR_CLASS_TYPE &a, const COLOR_CLASS_TYPE &b )  
{
	return COLOR_CLASS_TYPE( a[0] / b[0], a[1] / b[1], a[2] / b[2] );
}

// Scalar Addition (+)
inline friend COLOR_CLASS_TYPE operator+( const COLOR_CLASS_TYPE& a, const Scalar& d )  
{
	return COLOR_CLASS_TYPE( a[0] + d, a[1] + d, a[2] + d );
}

// Scalar Division (/) 
inline friend COLOR_CLASS_TYPE operator/( const COLOR_CLASS_TYPE &v, const Scalar t )  
{
	return COLOR_CLASS_TYPE( v[0]/t, v[1]/t, v[2]/t );
}

// Scalar Division (/) 
inline friend COLOR_CLASS_TYPE operator/( const Scalar t, const COLOR_CLASS_TYPE &v )  
{
	return COLOR_CLASS_TYPE( t/v[0], t/v[1], t/v[2] );
}

// Scalar Subtraction (-)
inline friend COLOR_CLASS_TYPE operator-( const Scalar &a, const COLOR_CLASS_TYPE &b )  
{
	return COLOR_CLASS_TYPE( a - b[0], a - b[1], a - b[2] );
}

// Scalar Subtraction (-)
inline friend COLOR_CLASS_TYPE operator-( const COLOR_CLASS_TYPE &b, const Scalar &a )  
{
	return COLOR_CLASS_TYPE( b[0] - a, b[1] - a, b[2] - a );
}

// less than scalar
inline friend bool operator<( const COLOR_CLASS_TYPE &b, const Scalar &a )  
{
	return (b[0]<a || b[1]<a || b[2]<a);
}

